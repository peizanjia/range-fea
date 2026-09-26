#include "typed_control_task.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QLockFile>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include <rbl_error.h>
#include <rml_model.h>
#include <rml_file_io.h>
#include <rparallel_execution_context.h>
#include <rstructural_typed_fe_control.h>

#include "control_typed_ndjson_transport.h"
#include "execution_event_journal.h"

namespace
{
QString targetPath(const QString &requested)
{
    QFileInfo file(requested);
    if (file.exists())
    {
        const auto path=file.canonicalFilePath();
        if (path.isEmpty() || !file.isFile()) throw std::invalid_argument("Typed checkpoint target is not a regular file.");
        return path;
    }
    const QString parent=file.dir().canonicalPath();
    if (parent.isEmpty() || file.isSymLink()) throw std::invalid_argument("Typed checkpoint parent is missing or target is a dangling link.");
    return QDir(parent).filePath(file.fileName());
}

QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not read typed control snapshot.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) throw std::runtime_error("Could not hash typed control snapshot.");
    return hash.result();
}

void require(bool condition,const char *message)
{
    if (!condition) throw std::invalid_argument(message);
}

void knownKeys(const QJsonObject &object,std::initializer_list<const char *> allowed)
{
    QSet<QString> names;
    for (const auto *name : allowed) names.insert(QString::fromLatin1(name));
    for (auto it=object.begin();it!=object.end();++it)
        require(names.contains(it.key()),"Unknown typed configuration key; check spelling and schema version.");
}

double real(const QJsonObject &object,const char *key,double fallback,bool required=false)
{
    if (!object.contains(key) && !required) return fallback;
    const auto value=object.value(key);
    require(value.isDouble() && std::isfinite(value.toDouble()),"Typed configuration requires a finite JSON number.");
    return value.toDouble();
}

uint index(const QJsonObject &object,const char *key,uint fallback=0,bool required=false)
{
    const double value=real(object,key,double(fallback),required);
    require(value>=0.0 && value<=std::numeric_limits<uint>::max() && value==std::floor(value),
            "Typed configuration index/count must be an unsigned integer.");
    return uint(value);
}

std::unique_ptr<RStructuralTypedFEControlDriver> initialize(
    const QString &modelPath,const QString &configurationPath,const QString &identity)
{
    QFile configuration(configurationPath);
    require(configuration.open(QIODevice::ReadOnly) && configuration.size()>0 && configuration.size()<=4*1024*1024,
            "Typed configuration is missing or exceeds 4 MiB.");
    QJsonParseError parseError;
    const auto document=QJsonDocument::fromJson(configuration.readAll(),&parseError);
    require(parseError.error==QJsonParseError::NoError && document.isObject(),"Invalid typed configuration JSON.");
    const auto config=document.object();
    knownKeys(config,{"version","mode","seed","dt","controller","channels","electricMode","patches"});
    require(index(config,"version",0,true)==1 && config["controller"].isString() && config["controller"].toString()==identity,
            "Typed configuration version or controller identity differs.");
    require(config["seed"].isString(),"Typed deterministic seed must be a decimal string.");
    bool seedOk=false;
    const quint64 seed=config["seed"].toString().toULongLong(&seedOk);
    require(seedOk,"Invalid typed deterministic seed.");
    const QString mode=config["mode"].toString();
    require(mode=="cable" || mode=="piezo_beam","Typed mode must be cable or piezo_beam.");
    require(config["channels"].isArray(),"Typed configuration lacks channels.");
    const auto channels=config["channels"].toArray();
    require(!channels.empty() && channels.size()<=4096,"Invalid typed channel count.");
    using Quantity=RStructuralTypedControlQuantity;
    const QMap<QString,Quantity> quantities={{"cable_reference_length",Quantity::CableReferenceLength},
        {"cable_initial_tension",Quantity::CableInitialTension},{"cable_anchor_displacement",Quantity::CableAnchorDisplacement},
        {"voltage",Quantity::Voltage},{"charge",Quantity::Charge}};
    std::vector<RStructuralTypedControlChannel> catalog;
    std::vector<RStructuralTypedFEControlBinding> bindings;
    for (const auto &value : channels)
    {
        require(value.isObject(),"Typed channel must be an object.");
        const auto item=value.toObject();
        knownKeys(item,{"id","quantity","initial","minimum","maximum","rate","delay","hold","noise","element","node","axis"});
        require(item["id"].isString() && item["quantity"].isString() && quantities.contains(item["quantity"].toString()),
                "Invalid typed channel ID or quantity.");
        RStructuralTypedControlChannel channel;
        channel.channelId=item["id"].toString();
        channel.quantity=quantities[item["quantity"].toString()];
        channel.initialCommand=real(item,"initial",0.0,true);
        channel.minimum=real(item,"minimum",-std::numeric_limits<double>::infinity());
        channel.maximum=real(item,"maximum",std::numeric_limits<double>::infinity());
        channel.rateLimit=real(item,"rate",std::numeric_limits<double>::infinity());
        channel.delaySteps=index(item,"delay");
        channel.holdSteps=index(item,"hold",1);
        channel.noiseAmplitude=real(item,"noise",0.0);
        catalog.push_back(channel);
        const bool anchor=channel.quantity==Quantity::CableAnchorDisplacement;
        bindings.push_back({channel.channelId,index(item,"element",0,!anchor),index(item,"node",0,anchor),index(item,"axis",0,anchor)});
    }
    RStructuralTypedControlSession session(seed,real(config,"dt",0.0,true),catalog,identity);
    // Native model links are rejected here: the locked/hashed input must itself
    // contain the model, rather than indirectly read an unbound mutable target.
    const bool ascii=QFileInfo(modelPath).suffix()==RModel::getDefaultFileExtension(false);
    require(ascii || QFileInfo(modelPath).suffix()==RModel::getDefaultFileExtension(true),"Typed initialization requires a native model.");
    RFile native(modelPath,ascii ? RFile::ASCII : RFile::BINARY);
    require(native.open(ascii ? QIODevice::ReadOnly|QIODevice::Text : QIODevice::ReadOnly),"Cannot open native typed input.");
    RFileHeader header;
    if (ascii) RFileIO::readAscii(native,header); else RFileIO::readBinary(native,header);
    require(header.getType()==R_FILE_TYPE_MODEL,"Typed initialization requires a complete native model; model links are unsupported.");
    native.close();
    RModel model;
    model.read(modelPath);
    if (mode=="cable")
    {
        require(!config.contains("patches") && !config.contains("electricMode"),"Cable configuration cannot contain piezo inputs.");
        return RStructuralTypedFEControlDriver::cable(model,session,bindings);
    }
    RStructuralPiezoBeamFEInput input;
    const QString electrical=config["electricMode"].toString();
    require(electrical=="voltage" || electrical=="charge","Piezo electricMode must be voltage or charge.");
    input.electricalMode=electrical=="voltage" ? RStructuralPiezoBeamElectricalMode::PrescribedVoltage : RStructuralPiezoBeamElectricalMode::PrescribedCharge;
    require(config["patches"].isArray() && !config["patches"].toArray().empty() && config["patches"].toArray().size()<=4096,
            "Invalid typed piezo patches.");
    for (const auto &value : config["patches"].toArray())
    {
        require(value.isObject(),"Piezo patch must be an object.");
        const auto item=value.toObject();
        knownKeys(item,{"element","area","thickness","offset","modulus","coupling","permittivity","initial"});
        RStructuralPiezoBeamPatch patch;
        patch.elementId=index(item,"element",0,true);
        patch.area=real(item,"area",0.0,true);
        patch.thickness=real(item,"thickness",0.0,true);
        patch.centroidOffsetY=real(item,"offset",0.0,true);
        patch.youngModulus=real(item,"modulus",0.0,true);
        patch.coupling=real(item,"coupling",0.0,true);
        patch.permittivity=real(item,"permittivity",0.0,true);
        patch.prescribedValue=real(item,"initial",0.0,true);
        input.patches.push_back(patch);
    }
    return RStructuralTypedFEControlDriver::piezoBeam(model,input,session,bindings);
}
}

int runTypedControlTask(const TypedControlTaskInput &input,QString &error)
{
    bool committed=false;
    std::unique_ptr<ExecutionEventJournal> journal;
    const auto append=[&](const QString &type,const QString &status,const QJsonObject &payload)
    {
        QString failure;
        if (!journal->append(type,status,payload,&failure))
            throw std::runtime_error(QString("Typed audit write failed: %1").arg(failure).toStdString());
    };
    try
    {
        if (input.steps<1 || input.steps>1000 || input.program.isEmpty() ||
            input.controllerIdentity.isEmpty() || input.timeoutMs<1 || input.timeoutMs>60000)
            throw std::invalid_argument("Invalid typed control run arguments.");
        if ((input.configurationFile.isEmpty() && QFileInfo(input.inputFile).suffix().compare("sfec",Qt::CaseInsensitive)!=0) ||
            QFileInfo(input.checkpointFile).suffix().compare("sfec",Qt::CaseInsensitive)!=0)
            throw std::invalid_argument("Typed continuation requires .sfec input; destination must always be .sfec.");
        const QString source=targetPath(input.inputFile);
        const QString destination=targetPath(input.checkpointFile);
        const QString configuration=input.configurationFile.isEmpty() ? QString() : targetPath(input.configurationFile);
        require(configuration.isEmpty() || (configuration!=source && configuration!=destination),"Typed config/model/checkpoint paths must be distinct.");
        require(configuration.isEmpty() || source!=destination,"Typed initialization must not overwrite the native source model.");
        QStringList lockPaths={source,destination};
        if (!configuration.isEmpty()) lockPaths.push_back(configuration);
        lockPaths.removeDuplicates();
        std::sort(lockPaths.begin(),lockPaths.end());
        std::vector<std::unique_ptr<QLockFile>> locks;
        for (const auto &path : lockPaths)
        {
            auto lock=std::make_unique<QLockFile>(path+".lock");
            lock->setStaleLockTime(0);
            if (!lock->tryLock(0)) throw std::runtime_error("Typed control snapshot is locked by another process.");
            locks.push_back(std::move(lock));
        }
        const QByteArray sourceHash=fileHash(source);
        const QByteArray configurationHash=configuration.isEmpty() ? QByteArray() : fileHash(configuration);
        const bool destinationExists=QFileInfo::exists(destination);
        const QByteArray destinationHash=destinationExists ? fileHash(destination) : QByteArray();
        RParallelExecutionConfig parallelConfig;
        parallelConfig.setMode(input.nThreads ? RParallelExecutionMode::FixedThreads : RParallelExecutionMode::SerialDeterministic);
        if (input.nThreads) parallelConfig.setRequestedThreads(std::max(1u,*input.nThreads));
        RParallelExecutionContext parallelContext(parallelConfig);
        auto driver=configuration.isEmpty() ? RStructuralTypedFEControlDriver::readCheckpoint(source,input.controllerIdentity) :
            initialize(source,configuration,input.controllerIdentity);
        if (fileHash(source)!=sourceHash || targetPath(input.inputFile)!=source ||
            (!configuration.isEmpty() && (fileHash(configuration)!=configurationHash || targetPath(input.configurationFile)!=configuration)))
            throw std::runtime_error("Typed control input changed during restoration.");
        journal=std::make_unique<ExecutionEventJournal>(destination+".events.ndjson",ExecutionEventJournal::createRunId());
        append("RunCreated","created",{{"mode","typed-quasistatic-fe"},{"protocolVersion",2},
            {"inputPath",source},{"inputSha256",QString::fromLatin1(sourceHash.toHex())},
            {"checkpointPath",destination},{"controllerIdentity",input.controllerIdentity},
            {"configurationSha256",QString::fromLatin1(configurationHash.toHex())},
            {"parallelPolicy",parallelConfig.toString()},
            {"actualThreads",int(parallelContext.getReport().actualThreads)},
            {"parallelFallback",parallelContext.getReport().fallback},
            {"parallelDiagnostic",parallelContext.getReport().diagnostic},
            {"deterministicReduction",parallelContext.getReport().deterministicReduction},
            {"stepsRequested",int(input.steps)}});
        const auto &session=driver->session();
        ControlTypedTransportConfig config;
        config.program=input.program;
        config.arguments=input.arguments;
        config.controllerIdentity=session.controllerIdentity();
        config.deterministicSeed=session.deterministicSeed();
        config.fixedDt=session.fixedDt();
        config.configurationSha256=QCryptographicHash::hash(sourceHash+configurationHash+session.configurationSha256(),QCryptographicHash::Sha256);
        config.timeoutMs=int(input.timeoutMs);
        for (const auto &channel : session.catalog()) config.channels.push_back(channel);
        for (const auto &sensor : driver->observation().sensors) config.sensorUnits.insert(sensor.sensorId,sensor.unit);
        ControlTypedNdjsonTransport transport(config);
        QString transportError;
        if (!transport.start(&transportError)) throw std::runtime_error(transportError.toStdString());
        for (uint i=0;i<input.steps;i++)
        {
            const quint64 stepId=driver->state().completedSteps+1;
            driver->advance([&](const RStructuralControlObservation &observation)
            {
                RStructuralTypedControlDecision decision;
                if (!transport.compute(stepId,observation,decision,&transportError))
                    throw std::runtime_error(transportError.toStdString());
                return decision;
            });
            QJsonArray channels;
            for (const auto &sample : driver->lastChannels())
                channels.append(QJsonObject{{"id",sample.channelId},{"quantity",int(sample.quantity)},
                    {"unit",rStructuralTypedControlUnit(sample.quantity)},
                    {"requested",sample.requestedValue},{"conditioned",sample.conditionedValue},
                    {"applied",sample.appliedValue},{"cumulativeWorkJ",sample.cumulativeWorkJ},
                    {"averagePowerW",sample.averagePowerW}});
            append("TypedStepPrepared","prepared",{{"stepId",QString::number(stepId)},
                {"acceptedTime",driver->state().acceptedTime},{"channels",channels}});
        }
        transport.stop();
        if (targetPath(input.checkpointFile)!=destination || targetPath(input.inputFile)!=source ||
            fileHash(source)!=sourceHash || QFileInfo::exists(destination)!=destinationExists ||
            (!configuration.isEmpty() && (fileHash(configuration)!=configurationHash || targetPath(input.configurationFile)!=configuration)) ||
            (destinationExists && fileHash(destination)!=destinationHash))
            throw std::runtime_error("Typed checkpoint input or destination changed before publication.");
        driver->writeCheckpoint(destination);
        committed=true;
        append("TypedCheckpointCommitted","committed",{{"checkpointPath",destination},
            {"checkpointSha256",QString::fromLatin1(fileHash(destination).toHex())},
            {"completedSteps",QString::number(driver->state().completedSteps)},
            {"acceptedTime",driver->state().acceptedTime}});
        append("RunFinished","succeeded",{{"checkpointCommitted",true}});
        error.clear();
        return 0;
    }
    catch (const RError &failure) { error=failure.getMessage(); }
    catch (const std::exception &failure) { error=QString::fromUtf8(failure.what()); }
    catch (...) { error="Unknown typed control failure."; }
    if (committed) error+=" Checkpoint committed; subsequent audit failed.";
    if (journal)
    {
        QString auditError;
        (void)journal->append("RunFinished","failed",{{"error",error},{"checkpointCommitted",committed}},&auditError);
    }
    return 1;
}
