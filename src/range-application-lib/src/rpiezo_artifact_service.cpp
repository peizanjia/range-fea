#include "rpiezo_artifact_service.h"
#include "piezo_native_backend_bridge.h"
#include "piezo_axial_backend_bridge.h"
#include <rstructural_piezo_layered_restart_codec.h>
#include <rstructural_piezo_axial_restart_codec.h>
#include <rml_model.h>
#include <rml_file.h>
#include <rml_file_io.h>
#include <rml_file_header.h>
#include <rbl_error.h>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryFile>
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
static_assert(std::is_nothrow_move_assignable_v<RPiezoArtifactService::Report>);
void require(bool ok,const char *message){if(!ok)throw std::invalid_argument(message);}
QString digest(const QByteArray &bytes){return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QString destination(const QString &path){
    const QFileInfo f(path);require(!path.isEmpty() && !f.isDir(),"Artifact destination is invalid");
    if(f.exists())return f.canonicalFilePath();
    const QString parent=QFileInfo(f.absolutePath()).canonicalFilePath();require(!parent.isEmpty(),"Artifact destination directory does not exist");
    return QDir(parent).filePath(f.fileName());
}
QString fileHash(const QString &path){
    QFile f(path);require(f.open(QIODevice::ReadOnly),"Cannot open native record");
    require(f.size()>0 && f.size()<=64*1024*1024,"Native record exceeds 64 MiB admission bound");
    QCryptographicHash hash(QCryptographicHash::Sha256);require(hash.addData(&f) && f.error()==QFileDevice::NoError,"Cannot hash native record");
    return QString::fromLatin1(hash.result().toHex());
}
void admit(const QString &path){
    const auto ext=QFileInfo(path).suffix().toLower();require(ext=="ram" || ext=="rbm","Native record must be RAM or RBM");
    RFile f(path,ext=="ram"?RFile::ASCII:RFile::BINARY);
    require(f.open(ext=="ram"?(QIODevice::ReadOnly|QIODevice::Text):QIODevice::ReadOnly),"Cannot open native record header");
    RFileHeader h;if(ext=="ram")RFileIO::readAscii(f,h);else RFileIO::readBinary(f,h);
    require(h.getType()==R_FILE_TYPE_MODEL,"Only explicit model records are admitted; resolve latest-record links first");
}
QByteArray number(double x){require(std::isfinite(x),"Artifact contains a nonfinite value");return QByteArray::number(x,'g',17);}
QByteArray cell(QString s){
    // All labels are prefixed with a physical/semantic category. Quoting also
    // handles source IDs containing delimiters; control characters are rejected.
    for(const QChar c:s)require(c.unicode()>=32 && c.unicode()!=127,"Artifact label contains a control character");
    s.replace('"',"\"\"");return '"'+s.toUtf8()+'"';
}
void put(QIODevice &f,const QByteArray &b){require(f.write(b)==b.size(),"Artifact write failed");}
void publish(QIODevice &source,const QString &path,const QByteArray &prefix={}){
    require(source.seek(0),"Cannot rewind verified artifact");QSaveFile out(path);out.setDirectWriteFallback(false);
    require(out.open(QIODevice::WriteOnly),"Cannot open artifact destination");put(out,prefix);
    while(!source.atEnd()){const auto b=source.read(1024*1024);require(!b.isEmpty(),"Artifact readback failed");put(out,b);}
    require(out.commit(),"Cannot atomically publish artifact");
}
QString failure(){try{throw;}catch(const RError &e){return e.getMessage();}catch(const std::exception &e){return QString::fromUtf8(e.what());}catch(...){return "Unknown artifact export failure";}}
QString layeredModelFamily(RStructuralPiezoLayeredModelFamily family)
{
    switch(family){
    case RStructuralPiezoLayeredModelFamily::PlanarBeam:return QStringLiteral("planarBeam");
    case RStructuralPiezoLayeredModelFamily::FlatPlate:return QStringLiteral("flatPlate");
    case RStructuralPiezoLayeredModelFamily::Solid3D:return QStringLiteral("solid3D");
    }
    throw std::invalid_argument("Unsupported validated layered model family");
}
struct Input{QString path,hash;uint step=0;};
struct Row{QStringList labels;QByteArray values;};
Row row(const RStructuralPiezoLayeredNativeRun &run){
    Row r;const auto &a=run.acceptedRuntime();const auto &s=run.acceptedState();
    const auto add=[&](const QString &label,double value){r.labels.append(label);if(!r.values.isEmpty())r.values+=',';r.values+=number(value);};
    add("completedFE [1]",a.completedFE);add("time [s]",s.integrator.state.time);add("controlTick [1]",a.controlTick);add("controlPhase [1]",a.controlPhase);
    const auto &ids=run.adapter().mesh().nodeIds;
    const char *components[]={"X","Y","Z","RX","RY","RZ"};
    const std::vector<double> *vectors[]={&s.physicalDisplacements,&s.physicalVelocities,&s.physicalAccelerations,&s.physicalReactions};
    const char *quantities[]={"q.absolute","v","a","reaction"};
    for(uint kind=0;kind<4;++kind){require(vectors[kind]->size()==6*ids.size(),"Physical artifact vector shape differs from node map");
        for(size_t i=0;i<ids.size();++i)for(uint c=0;c<6;++c){const QString unit=kind==0?(c<3?"m":"rad"):kind==1?(c<3?"m/s":"rad/s"):kind==2?(c<3?"m/s^2":"rad/s^2"):(c<3?"N":"N*m");
            add(QString("node:%1:%2:%3 [%4]").arg(ids[i]).arg(components[c],quantities[kind],unit),(*vectors[kind])[6*i+c]);}}
    const auto &d=run.adapter().data();const auto &mesh=run.adapter().mesh();
    require(d.generalizedCoordinateUnits.size()==d.fullDofs,"Mapped DOF units are missing");
    const auto mappedQ=mesh.projectPhysical(s.physicalDisplacements,RStructuralPiezoPhysicalDomain::Displacement);
    const auto mappedV=mesh.projectPhysical(s.physicalVelocities,RStructuralPiezoPhysicalDomain::Velocity);
    const auto mappedA=mesh.projectPhysical(s.physicalAccelerations,RStructuralPiezoPhysicalDomain::Velocity);
    for(uint i=0;i<d.fullDofs;++i){const auto &unit=d.generalizedCoordinateUnits[i];add(QString("mappedDof:%1:q0 [%2]").arg(i).arg(unit),d.q0[i]);add(QString("mappedDof:%1:q.absolute [%2]").arg(i).arg(unit),mappedQ[i]);add(QString("mappedDof:%1:v [%2/s]").arg(i).arg(unit),mappedV[i]);add(QString("mappedDof:%1:a [%2/s^2]").arg(i).arg(unit),mappedA[i]);}
    for(size_t i=0;i<s.integrator.state.q.size();++i){add(QString("reducedDof:%1:eta [mixed-SI; see P,T]").arg(i),s.integrator.state.q[i]);add(QString("reducedDof:%1:etaVelocity [mixed-SI/s; see P,T]").arg(i),s.integrator.state.v[i]);add(QString("reducedDof:%1:etaAcceleration [mixed-SI/s^2; see P,T]").arg(i),s.integrator.state.a[i]);}
    for(const auto &p:s.electrical.ports){const QString base="port:"+p.portId;
        add(base+":voltage.absolute [V]",p.voltage);add(base+":voltage.delta [V]",p.deltaVoltage);add(base+":charge.absolute [C]",p.charge);add(base+":charge.delta [C]",p.deltaCharge);
        add(base+":amplifierVoltage.absolute [V]",p.amplifierVoltage);add(base+":amplifierVoltage.delta [V]",p.deltaAmplifierVoltage);}
    for(const auto &p:s.readouts){add("readout:"+p.portId+":absolute ["+p.unit+"]",p.value);add("readout:"+p.portId+":delta ["+p.unit+"]",p.deltaValue);}
    require(a.actions.empty() || a.actions.size()==s.heldActuatorVoltages.size(),"Action table shape differs from actuator map");
    const auto &actuators=run.adapter().electrical().condensed().actuatorPortIds;
    for(size_t i=0;i<actuators.size();++i){const QString base="action:"+actuators[i];add(base+":heldRequested [V]",a.heldRequested[i]);add(base+":applied [V]",a.lastApplied[i]);
        // Initial accepted state has no previous interval action.
        add(base+":intervalPresent [1]",a.actions.empty()?0:1);
        const RStructuralPiezoLayeredVoltageAction initial{actuators[i],a.heldRequested[i],a.lastApplied[i],a.lastApplied[i],a.lastApplied[i],false,false};
        const auto &v=a.actions.empty()?initial:a.actions[i];add(base+":amplitudeLimited [V]",v.amplitudeLimited);add(base+":previousApplied [V]",v.previousApplied);add(base+":amplitudeClipped [1]",v.amplitudeClipped);add(base+":rateClipped [1]",v.rateClipped);}
    add("energy:kinetic [J]",s.kineticEnergy);add("energy:stored [J]",s.storedEnergy);add("energy:initial [J]",s.initialEnergy);add("work:backgroundExternal [J]",s.backgroundExternalWork);add("work:electrical [J]",s.electricalWork);add("work:viscousDissipation [J]",s.viscousDissipation);add("energy:balanceResidual [J]",s.energyBalanceResidual);add("force:freeResidual [mixed-SI-norm]",s.freeForceResidual);
    r.values+='\n';return r;
}
Row row(const RStructuralPiezoAxialNativeRun &run) {
    Row r;const auto &a=run.acceptedRuntime();const auto &s=run.acceptedState();
    const auto add=[&](const QString &label,double value){r.labels.append(label);if(!r.values.isEmpty())r.values+=',';r.values+=number(value);};
    add("completedFE [1]",a.completedFE);add("time [s]",s.time);add("controlTick [1]",a.controlTick);add("controlPhase [1]",a.controlPhase);
    const auto &ids=run.adapter().maps().orderedPhysicalNodeIds;
    const char *components[]={"X","Y","Z"};const char *quantities[]={"q.absolute","v","a","reaction","externalForce"};
    const char *units[]={"m","m/s","m/s^2","N","N"};
    const std::vector<double> *vectors[]={&s.displacements,&s.velocities,&s.accelerations,&s.reactions,&s.appliedInputs.externalForces};
    for(uint kind=0;kind<5;++kind){require(vectors[kind]->size()==3*ids.size(),"Axial physical artifact vector shape differs");
        for(size_t i=0;i<ids.size();++i)for(uint c=0;c<3;++c)
            add(QString("node:%1:%2:%3 [%4]").arg(ids[i]).arg(components[c],quantities[kind],units[kind]),(*vectors[kind])[3*i+c]);}
    const auto &q0=run.adapter().piezoReadout().reference().displacements;
    for(size_t i=0;i<q0.size();++i){add(QString("physicalDof:%1:q0 [m]").arg(i),q0[i]);add(QString("physicalDof:%1:eta [m]").arg(i),s.displacements[i]-q0[i]);}
    for(const auto &p:a.readouts){add("port:"+p.portId+":voltage.absolute [V]",p.voltage);add("port:"+p.portId+":voltage.delta [V]",p.deltaVoltage);
        add("port:"+p.portId+":charge.absolute [C]",p.charge);add("port:"+p.portId+":charge.delta [C]",p.deltaCharge);
        add("readout:"+p.portId+":absolute ["+p.unit+"]",p.value);add("readout:"+p.portId+":delta ["+p.unit+"]",p.deltaValue);}
    size_t column=0;
    for(const auto &id:run.adapter().maps().orderedPortIds){const auto &catalog=run.adapter().piezoReadoutSetup().ports;
        const auto port=std::find_if(catalog.begin(),catalog.end(),[&](const auto &p){return p.portId==id;});
        require(port!=catalog.end(),"Axial artifact port metadata missing");if(port->role==RStructuralPiezoPortRole::Sensor)continue;
        const auto base="action:"+id;add(base+":heldRequested [V]",a.heldRequested.at(column));add(base+":applied [V]",a.lastApplied.at(column));
        add(base+":intervalPresent [1]",a.actions.empty()?0:1);
        const RStructuralPiezoAxialVoltageAction initial{id,a.heldRequested[column],a.lastApplied[column],a.lastApplied[column],a.lastApplied[column],false,false};
        const auto &v=a.actions.empty()?initial:a.actions.at(column);add(base+":amplitudeLimited [V]",v.amplitudeLimited);add(base+":previousApplied [V]",v.previousApplied);
        add(base+":amplitudeClipped [1]",v.amplitudeClipped);add(base+":rateClipped [1]",v.rateClipped);++column;
    }
    add("energy:kinetic [J]",s.kineticEnergy);add("energy:stored [J]",s.storedEnergy);add("energy:initial [J]",s.initialTotalEnergy);
    add("work:external [J]",s.cumulativeExternalWork);add("work:support [J]",s.cumulativeSupportWork);add("work:electrical [J]",s.cumulativeElectricalWork);
    add("work:cableLength [J]",s.cumulativeLengthWork);add("work:viscousDissipation [J]",s.cumulativeDampingDissipation);
    add("energy:balanceResidual [J]",s.kineticEnergy+s.storedEnergy-s.initialTotalEnergy-s.cumulativeExternalWork-s.cumulativeSupportWork-s.cumulativeElectricalWork-s.cumulativeLengthWork+s.cumulativeDampingDissipation);
    r.values+='\n';return r;
}
RPiezoArtifactService::Report axialTimeSeries(const QStringList &paths,const QString &target,const QString &expectedSource) {
    std::vector<Input> inputs;std::set<QString> names;QString source;QByteArray config;std::unique_ptr<RModel> first;
    for(const auto &path:paths){const QFileInfo f(path);require(f.isFile(),"Native record does not exist");const auto canonical=f.canonicalFilePath();
        require(!canonical.isEmpty() && canonical.compare(target,Qt::CaseInsensitive)!=0,"CSV destination would overwrite native input");
        require(names.insert(canonical.toCaseFolded()).second,"Duplicate native input path");Input in;in.path=canonical;in.hash=fileHash(canonical);admit(canonical);
        auto model=std::make_unique<RModel>();model->read(canonical);require(fileHash(canonical)==in.hash,"Native input changed during read");
        const auto &restart=model->getProblemSetup().getStructuralPiezoAxialRestart();restart.validate();require(restart.available,"Native input has no complete accepted axial state");
        const auto state=RStructuralPiezoAxialRestartCodec::decode(restart.runtimeState);const auto &setup=model->getProblemSetup().getStructuralPiezoAxialSetup();setup.validate(*model);
        require(state.sourceFingerprint==setup.computeSourceFingerprint(*model) && restart.sourceFingerprint==state.sourceFingerprint,"Native axial source differs from accepted state");
        if(inputs.empty()){source=state.sourceFingerprint;config=state.configurationIdentity;first=std::move(model);}else require(source==state.sourceFingerprint && config==state.configurationIdentity,"Native records differ in source or configuration");
        require(expectedSource.isEmpty() || source==expectedSource,"Native records differ from the selected experiment source");in.step=state.completedFE;inputs.push_back(in);
    }
    std::sort(inputs.begin(),inputs.end(),[](const auto &a,const auto &b){return a.step<b.step;});
    for(size_t i=1;i<inputs.size();++i)require(inputs[i].step!=inputs[i-1].step,"Duplicate accepted FE step");
    const auto initial=RStructuralPiezoAxialRestartCodec::decode(first->getProblemSetup().getStructuralPiezoAxialRestart().runtimeState);
    RStructuralPiezoAxialRuntimeCallbacks callbacks;if(initial.backendIdentity!="range-fea/native-axial-Newmark/no-control/v1")callbacks=PiezoAxialBackendBridge::create(*first).callbacks;
    QTemporaryFile temp;require(temp.open(),"Cannot stage accepted axial CSV");QCryptographicHash expectedPayload(QCryptographicHash::Sha256);
    QStringList labels;QJsonArray provenance,nodes,readouts;QString backend;RPiezoArtifactService::Report result;
    result.sourceFingerprint=source;result.modelFamily=QStringLiteral("axialRod");result.recordCount=uint(inputs.size());result.continuousFE=true;result.qualification="sampled-certified-accepted-native-records";
    for(size_t index=0;index<inputs.size();++index){const auto &input=inputs[index];require(fileHash(input.path)==input.hash,"Native input changed before certification");
        RModel model;model.read(input.path);model.getProblemSetup().setRestart(true);RStructuralPiezoAxialNativeRun restored(model,callbacks);
        require(restored.acceptedRuntime().configurationIdentity==config && restored.acceptedRuntime().sourceFingerprint==source,"Certified axial identity differs");
        require(fileHash(input.path)==input.hash,"Native input changed during certification");const auto output=row(restored);const double time=restored.acceptedState().time;
        if(index==0){labels=output.labels;QByteArray header;for(const auto &label:labels){if(!header.isEmpty())header+=',';header+=cell(label);}header+='\n';put(temp,header);expectedPayload.addData(header);
            result.firstTime=time;backend=restored.acceptedRuntime().backendIdentity;for(uint id:restored.adapter().maps().orderedPhysicalNodeIds)nodes.append(double(id));
            for(const auto &p:restored.acceptedRuntime().readouts)readouts.append(QJsonObject{{"portId",p.portId},{"unit",p.unit},{"mode",int(p.mode)},{"actionRecord",p.actionRecord}});
        }else {require(output.labels==labels && time>result.lastTime,"Certified axial columns or time ordering differ");if(input.step!=inputs[index-1].step+1)result.continuousFE=false;}
        put(temp,output.values);expectedPayload.addData(output.values);result.lastTime=time;provenance.append(QJsonObject{{"fileName",QFileInfo(input.path).fileName()},{"sha256",input.hash},{"completedFE",double(input.step)}});
    }
    require(temp.flush() && temp.seek(0),"Cannot read back accepted axial CSV");QCryptographicHash payload(QCryptographicHash::Sha256);
    require(payload.addData(&temp) && payload.result()==expectedPayload.result(),"Accepted axial CSV readback differs");
    const QJsonObject meta{{"schema","range.piezo-accepted-timeseries/v1"},{"family","axialRod"},{"modelFamily",result.modelFamily},{"qualification",result.qualification},{"identityScope","compatibility-not-authentication"},
        {"sourceFingerprint",source},{"configurationIdentity",QString::fromLatin1(config.toHex())},{"backendIdentity",backend},{"recordCount",double(result.recordCount)},
        {"firstTime",result.firstTime},{"lastTime",result.lastTime},{"continuousBetweenSelectedRecords",result.continuousFE},{"coverage","exact selected accepted records; no interpolation or claim of complete FE history"},
        {"records",provenance},{"csvPayloadSha256",QString::fromLatin1(payload.result().toHex())},{"physicalNodeIds",nodes},{"readouts",readouts},{"coordinateConvention","physical XYZ absolute; increments q-q0; columns include SI units"}};
    for(const auto &input:inputs)require(fileHash(input.path)==input.hash,"Native input changed before publication");
    require(expectedSource.isEmpty() || source==expectedSource,"Selected experiment source differs before publication");
    const auto prefix="# "+QJsonDocument(meta).toJson(QJsonDocument::Compact)+'\n';publish(temp,target,prefix);return result;
}
}
bool RPiezoArtifactService::writeMatrixPackage(const RModel &model,const QString &path,QString *error,Report *report){
    if(error)error->clear();try{
        const auto target=destination(path);require(QFileInfo(target).suffix().compare("json",Qt::CaseInsensitive)==0,"Matrix export requires a JSON destination; native model files are protected");
        const bool axial=model.getProblemSetup().getStructuralPiezoAxialSetup().enabled;
        const auto &setup=model.getProblemSetup().getStructuralPiezoLayeredSetup();
        QString source,modelFamily;QByteArray configuration;QJsonObject package;QJsonArray coordinateUnits,forceUnits;
        if(axial) {
            const auto &declaration=model.getProblemSetup().getStructuralPiezoAxialSetup();declaration.validate(model);
            modelFamily=QStringLiteral("axialRod");
            source=declaration.computeSourceFingerprint(model);configuration=declaration.identityBytes();
            package=PiezoAxialBackendBridge::create(model).exportPackage;
            for(uint i=0;i<3*model.getNNodes();++i){coordinateUnits.append("m");forceUnits.append("N");}
        } else {
            setup.validate(model);source=setup.computeSourceFingerprint(model);configuration=setup.identityBytes();
            modelFamily=layeredModelFamily(setup.family);
            const auto bridge=PiezoNativeBackendBridge::create(model);package=bridge.exportPackage;
            RStructuralPiezoLayeredModelAdapter adapter(model,setup,bridge.callbacks.authoritativeFullC);
            for(const auto &unit:adapter.data().generalizedCoordinateUnits)coordinateUnits.append(unit);
            for(const auto &unit:adapter.data().generalizedForceUnits)forceUnits.append(unit);
        }
        require(package.value("sourceFingerprint").toString()==source,"Matrix package source identity differs from current model");
        package["artifactQualification"]="actual-source-bound-native-FE-operators";
        package["sourceConfigurationSha256"]=digest(configuration);
        package["forceContract"]=QJsonObject{{"Bforce","maps configured perturbation force channels; source background is separate"},{"biasForce","backgroundReducedOffset: compiled background force minus reference force, reduced by T transpose"},{"Dforce","total force readout feedthrough La*M^-1*Bforce; initialization additional feedthrough was zero"},{"nativePerturbationForce","callback excludes compiled source backgroundForce"}};
        package["units"]=QJsonObject{{"system","SI"},{"time","s"},{"voltage","V"},{"charge","C"},{"energyAndWork","J"},{"physicalTranslation","m"},{"physicalRotation","rad"},{"physicalForce","N"},{"physicalMoment","N*m"},{"mapping","qPhysical=P*(T*z+p); reduced increments eta=z-z0; mixed units require coordinate maps"}};
        if(axial)package["units"]=QJsonObject{{"system","SI"},{"time","s"},{"voltage","V"},{"charge","C"},{"energyAndWork","J"},{"physicalTranslation","m"},{"physicalForce","N"},{"mapping","physical XYZ; free selection T; reduced increments q-q0"}};
        package["mappedCoordinateUnits"]=coordinateUnits;package["mappedForceUnits"]=forceUnits;
        const QByteArray bytes=QJsonDocument(package).toJson(QJsonDocument::Indented);QTemporaryFile temp;require(temp.open(),"Cannot stage matrix package");put(temp,bytes);require(temp.flush() && temp.seek(0),"Cannot read back matrix package");
        require(QJsonDocument::fromJson(temp.readAll()).object()==package,"Matrix package readback differs from actual export");
        Report result;result.sourceFingerprint=source;result.modelFamily=modelFamily;result.qualification="actual-source-bound-native-FE-operators";
        const auto current=axial?model.getProblemSetup().getStructuralPiezoAxialSetup().computeSourceFingerprint(model):setup.computeSourceFingerprint(model);
        require(current==source,"Source changed during matrix export");publish(temp,target);
        if(report)*report=std::move(result);return true;
    }catch(...){if(error)*error=failure();return false;}
}
bool RPiezoArtifactService::writeAcceptedTimeSeries(const QStringList &paths,const QString &csvPath,QString *error,Report *report,const QString &expectedSourceFingerprint){
    if(error)error->clear();try{
        require(!paths.isEmpty() && paths.size()<=20001,"Select between 1 and 20001 explicit accepted records");const auto target=destination(csvPath);
        require(QFileInfo(target).suffix().compare("csv",Qt::CaseInsensitive)==0,"Time series export requires a CSV destination; native model files are protected");
        admit(paths.front());RModel family;family.read(paths.front());
        if(family.getProblemSetup().getStructuralPiezoAxialSetup().enabled) {
            auto result=axialTimeSeries(paths,target,expectedSourceFingerprint);if(report)*report=std::move(result);return true;
        }
        std::vector<Input> inputs;std::set<QString> names;QString source,modelFamily;QByteArray config;std::unique_ptr<RModel> first;
        for(const auto &path:paths){const QFileInfo f(path);require(f.isFile(),"Native record does not exist");const auto canonical=f.canonicalFilePath();
            require(!canonical.isEmpty() && canonical.compare(target,Qt::CaseInsensitive)!=0,"CSV destination would overwrite a native input");require(names.insert(canonical.toCaseFolded()).second,"Duplicate native input path");
            Input in;in.path=canonical;in.hash=fileHash(canonical);admit(canonical);auto model=std::make_unique<RModel>();model->read(canonical);
            require(fileHash(canonical)==in.hash,"Native input changed during read");const auto &restart=model->getProblemSetup().getStructuralPiezoLayeredRestart();restart.validate();require(restart.available,"Native input has no complete accepted layered state");
            const auto state=RStructuralPiezoLayeredRestartCodec::decode(restart.stateBytes);const auto &setup=model->getProblemSetup().getStructuralPiezoLayeredSetup();setup.validate(*model);
            require(state.sourceFingerprint==setup.computeSourceFingerprint(*model) && restart.sourceFingerprint==state.sourceFingerprint,"Native input source no longer matches its accepted state");
            const auto actualModelFamily=layeredModelFamily(setup.family);
            if(inputs.empty()){source=state.sourceFingerprint;config=state.configurationIdentity;modelFamily=actualModelFamily;first=std::move(model);}
            else require(state.sourceFingerprint==source && state.configurationIdentity==config && actualModelFamily==modelFamily,"Native records differ in source, runtime configuration or model family");
            require(expectedSourceFingerprint.isEmpty() || source==expectedSourceFingerprint,"Native records differ from the selected experiment source");
            in.step=state.completedFE;inputs.push_back(in);
        }
        std::sort(inputs.begin(),inputs.end(),[](const Input &a,const Input &b){return a.step<b.step;});
        for(size_t i=1;i<inputs.size();++i)require(inputs[i].step!=inputs[i-1].step,"Duplicate accepted FE step");
        const auto initial=RStructuralPiezoLayeredRestartCodec::decode(first->getProblemSetup().getStructuralPiezoLayeredRestart().stateBytes);
        RStructuralPiezoLayeredRuntimeCallbacks callbacks;
        if(initial.backendIdentity!="range-fea/native-layered-Newmark/no-control/v1")callbacks=PiezoNativeBackendBridge::create(*first).callbacks;
        QTemporaryFile temp;require(temp.open(),"Cannot stage accepted CSV");QCryptographicHash expectedPayload(QCryptographicHash::Sha256);QStringList labels;QJsonArray provenance;Report result;result.sourceFingerprint=source;result.modelFamily=modelFamily;result.recordCount=uint(inputs.size());result.continuousFE=true;result.qualification="sampled-certified-accepted-native-records";
        QJsonArray nodeMap,mappedUnits,transform,freeTransform,readoutMap;QString backend;
        for(size_t index=0;index<inputs.size();++index){const auto &input=inputs[index];require(fileHash(input.path)==input.hash,"Native input changed before certification");RModel model;model.read(input.path);model.getProblemSetup().setRestart(true);
            RStructuralPiezoLayeredNativeRun restored(model,callbacks);require(restored.acceptedRuntime().configurationIdentity==config && restored.acceptedRuntime().sourceFingerprint==source,"Certified native source identity differs");
            require(fileHash(input.path)==input.hash,"Native input changed during certification");auto output=row(restored);
            const double time=restored.acceptedState().integrator.state.time;if(index==0){labels=output.labels;QByteArray header;for(const auto &label:labels){if(!header.isEmpty())header+=',';header+=cell(label);}header+='\n';put(temp,header);expectedPayload.addData(header);result.firstTime=time;backend=restored.acceptedRuntime().backendIdentity;
                const auto &mesh=restored.adapter().mesh();const auto &d=restored.adapter().data();for(auto id:mesh.nodeIds)nodeMap.append(double(id));for(const auto &unit:d.generalizedCoordinateUnits)mappedUnits.append(unit);
                for(const auto &readout:restored.acceptedState().readouts)readoutMap.append(QJsonObject{{"portId",readout.portId},{"unit",readout.unit},{"mode",int(readout.mode)},{"actionRecord",readout.actionRecord}});
                const auto matrix=[](const std::vector<double> &v,size_t rows,size_t cols){QJsonArray out;for(size_t i=0;i<rows;++i){QJsonArray r;for(size_t j=0;j<cols;++j){number(v[i*cols+j]);r.append(v[i*cols+j]);}out.append(r);}return out;};
                transform=matrix(mesh.physicalTransform,6*mesh.nodeIds.size(),d.fullDofs);freeTransform=matrix(d.T,d.fullDofs,d.freeDofs);
            }else{require(output.labels==labels && time>result.lastTime,"Certified CSV columns or time ordering differ");if(input.step!=inputs[index-1].step+1)result.continuousFE=false;}
            put(temp,output.values);expectedPayload.addData(output.values);result.lastTime=time;provenance.append(QJsonObject{{"fileName",QFileInfo(input.path).fileName()},{"sha256",input.hash},{"completedFE",double(input.step)}});
        }
        require(temp.flush() && temp.seek(0),"Cannot read back accepted CSV");QCryptographicHash payload(QCryptographicHash::Sha256);require(payload.addData(&temp),"Cannot verify accepted CSV payload");
        require(payload.result()==expectedPayload.result(),"Accepted CSV readback differs from certified rows");
        QJsonObject meta{{"schema","range.piezo-accepted-timeseries/v1"},{"family",result.modelFamily},{"modelFamily",result.modelFamily},{"qualification",result.qualification},{"identityScope","compatibility-not-authentication"},{"sourceFingerprint",source},{"configurationIdentity",QString::fromLatin1(config.toHex())},{"backendIdentity",backend},{"recordCount",double(result.recordCount)},{"firstTime",result.firstTime},{"lastTime",result.lastTime},{"continuousBetweenSelectedRecords",result.continuousFE},{"coverage","exact selected accepted records; no interpolation or claim of complete FE history"},{"records",provenance},{"csvPayloadSha256",QString::fromLatin1(payload.result().toHex())},{"physicalNodeIds",nodeMap},{"mappedCoordinateUnits",mappedUnits},{"P",transform},{"T",freeTransform},{"coordinateConvention","physical q absolute; reduced eta is increment about z0; columns include SI units"}};
        meta["readouts"]=readoutMap;
        for(const auto &input:inputs)require(fileHash(input.path)==input.hash,"Native input changed before publication");
        require(expectedSourceFingerprint.isEmpty() || source==expectedSourceFingerprint,"Selected experiment source differs before publication");
        const auto prefix="# "+QJsonDocument(meta).toJson(QJsonDocument::Compact)+'\n';
        publish(temp,target,prefix);if(report)*report=std::move(result);return true;
    }catch(...){if(error)*error=failure();return false;}
}
bool RPiezoArtifactService::writeAcceptedTraceTimeSeries(const PiezoResultSeries::TraceProof &proof,const QString &csvPath,QString *error,Report *report,const std::function<void()> &finalAuthorityCheck)
{
    if(error)error->clear();try {
        const auto target=destination(csvPath);require(QFileInfo(target).suffix().compare("csv",Qt::CaseInsensitive)==0,"Accepted trace export requires a CSV destination; native sources are protected");
        const auto base=QFileInfo(proof.baseModelPath).canonicalFilePath(),trace=QFileInfo(proof.tracePath).canonicalFilePath();
        require(!base.isEmpty()&&!trace.isEmpty()&&target.compare(base,Qt::CaseInsensitive)!=0&&target.compare(trace,Qt::CaseInsensitive)!=0,"CSV destination would overwrite the approved base MODEL or trace");
        QSaveFile output(target);output.setDirectWriteFallback(false);require(output.open(QIODevice::WriteOnly),"Cannot stage atomic accepted trace CSV");
        QTemporaryFile payload;require(payload.open(),"Cannot stage streaming accepted trace CSV payload");QCryptographicHash payloadHash(QCryptographicHash::Sha256);bool first=true;
        const auto real=[](double value){require(std::isfinite(value),"Trace CSV contains a nonfinite value");return value==0&&std::signbit(value)?QByteArray("-0"):QByteArray::number(value,'g',17);};
        const auto result=PiezoResultSeries::streamAcceptedTrace(proof,{},[&](const auto &frame,const auto &channels){
            if(first){QByteArray header="\"completedFE [1]\",\"time [s]\",\"controlTick [1]\",\"controlPhase [1]\",\"stateSha256\"";
                for(const auto &channel:channels)header+=','+cell(channel.channelId+" ["+channel.unit+"]");header+='\n';put(payload,header);payloadHash.addData(header);first=false;
            }
            QByteArray line=QByteArray::number(frame.completedFE)+','+real(frame.timeS)+','+QByteArray::number(frame.controlTick)+','+QByteArray::number(frame.controlPhase)+','+cell(frame.stateSha256);
            require(frame.channels.size()==channels.size(),"Trace CSV channel catalog shape changed");for(qsizetype i=0;i<channels.size();++i){require(frame.channels[i].channelId==channels[i].channelId&&frame.channels[i].unit==channels[i].unit,"Trace CSV channel order or unit changed");line+=','+real(frame.channels[i].value);}line+='\n';put(payload,line);payloadHash.addData(line);
        },[&](const auto &dataset){
            require(!first&&payload.flush()&&payload.seek(0),"Trace CSV payload could not be read back");QCryptographicHash readback(QCryptographicHash::Sha256);require(readback.addData(&payload)&&readback.result()==payloadHash.result(),"Trace CSV readback differs from certified streaming rows");
            QJsonArray channels,readouts;for(const auto &channel:dataset.channels)channels.append(QJsonObject{{"channelId",channel.channelId},{"quantity",channel.quantity},{"axis",channel.axis},{"unit",channel.unit},{"nodeTarget",channel.nodeTarget},{"portId",channel.portId},{"physicalNodeId",double(channel.physicalNodeId)},{"actionRecord",channel.actionRecord}});
            for(const auto &readout:dataset.readouts)readouts.append(QJsonObject{{"portId",readout.portId},{"unit",readout.unit},{"mode",readout.mode},{"actionRecord",readout.actionRecord},{"amplifierAvailable",readout.amplifierAvailable}});
            const QJsonObject metadata{{"schema","range.piezo-accepted-trace-timeseries/v1"},{"family",dataset.family},{"modelFamily",dataset.modelFamily},{"qualification","certified-committed-native-trace-prefix"},{"identityScope","compatibility-not-authentication"},
                {"sourceFingerprint",dataset.sourceFingerprint},{"configurationIdentity",dataset.configurationIdentity},{"backendIdentity",dataset.backendIdentity},{"artifactSha256",dataset.artifactSha256},
                {"baseModelSha256",QString::fromLatin1(proof.baseModelSha256.toHex())},{"headerSha256",QString::fromLatin1(proof.expectedHeaderSha256.toHex())},{"contextSha256",QString::fromLatin1(proof.expectedContextSha256.toHex())},
                {"baseSeq",double(dataset.baseSeq)},{"targetSteps",double(dataset.targetSteps)},{"initialTime",dataset.initialTimeS},{"dt",dataset.dtS},{"recordCount",dataset.times.size()},
                {"firstTime",dataset.times.first()},{"lastTime",dataset.times.last()},{"continuousBetweenSelectedRecords",true},{"sampledCoverage",dataset.sampledCoverage},{"coverage",dataset.coverage},{"channels",channels},{"readouts",readouts},
                {"committedCursor",QJsonObject{{"byteEnd",double(proof.cursor.byteEnd)},{"sequence",double(proof.cursor.sequence)},{"lastRecordOffset",double(proof.cursor.lastRecordOffset)},{"recordSha256",QString::fromLatin1(proof.cursor.recordSha32.toHex())},{"stateSha256",QString::fromLatin1(proof.cursor.stateSha32.toHex())}}},
                {"csvPayloadSha256",QString::fromLatin1(payloadHash.result().toHex())},{"coordinateConvention","actual certified physical components and named electrode channels; raw binary64 values, no interpolation; complete opaque runtime remains in the committed trace"}};
            put(output,"# "+QJsonDocument(metadata).toJson(QJsonDocument::Compact)+'\n');require(payload.seek(0),"Trace CSV payload seek failed");
            while(!payload.atEnd()){const auto bytes=payload.read(64*1024);require(!bytes.isEmpty(),"Trace CSV payload read failed");put(output,bytes);}
        });
        require(result.valid&&bool(result.dataset),qPrintable(result.message));
        Report receipt;receipt.sourceFingerprint=result.dataset->sourceFingerprint;receipt.modelFamily=result.dataset->modelFamily;receipt.recordCount=uint(result.dataset->times.size());receipt.firstTime=result.dataset->times.first();receipt.lastTime=result.dataset->times.last();receipt.continuousFE=true;receipt.qualification="certified-committed-native-trace-prefix";
        if(finalAuthorityCheck)finalAuthorityCheck();
        require(output.commit(),"Atomic accepted trace CSV publication failed");if(report)*report=std::move(receipt);return true;
    }catch(...){if(error)*error=failure();return false;}
}
