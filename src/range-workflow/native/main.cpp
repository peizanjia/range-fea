#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <rbl_error.h>
#include <rbl_utils.h>
#include <rml_model.h>
#include <rml_file_io.h>
#include <rml_file_utils.h>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>
#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
QByteArray digest(const QString &path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Cannot hash input.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), "Cannot read input for hash."); return hash.result().toHex();
}
struct Source {
    QString model;
    std::vector<std::pair<QString,QByteArray>> files;
    QJsonArray references() const {
        QJsonArray refs;
        for(const auto &[path,sha]:files) refs.append(QJsonObject{{"path",path},{"sha256",QString::fromLatin1(sha)}});
        return refs;
    }
    void verify() const {
        for(const auto &[path,sha]:files) require(digest(path)==sha,"A model or native link changed during the operation.");
    }
};
Source resolveSource(const QString &initial) {
    Source source;
    QSet<QString> visited;
    QString current=initial;
    const auto directory=QFileInfo(initial).dir().canonicalPath();
    for(int depth=0;depth<16;++depth) {
        QFileInfo info(current);
        require(info.isFile() && !info.isSymLink() && info.dir().canonicalPath()==directory,
                "Native link must resolve to a regular file in the input directory.");
        current=info.canonicalFilePath();
        require(!visited.contains(current),"Cyclic native model link."); visited.insert(current);
        const bool ascii=info.suffix()==RModel::getDefaultFileExtension(false);
        require(ascii || info.suffix()==RModel::getDefaultFileExtension(true),"Expected .ram or .rbm input.");
        source.files.emplace_back(current,digest(current));
        RFile file(current,ascii?RFile::ASCII:RFile::BINARY);
        require(file.open(ascii?QIODevice::ReadOnly|QIODevice::Text:QIODevice::ReadOnly),"Cannot read model header.");
        RFileHeader header;
        if(ascii) RFileIO::readAscii(file,header); else RFileIO::readBinary(file,header);
        if(header.getType()==R_FILE_TYPE_MODEL) {source.model=current; source.verify(); return source;}
        require(header.getType()==R_FILE_TYPE_LINK,"Input is neither a complete model nor a native link.");
        current=RFileUtils::findLinkTargetFileName(current,header.getInformation());
    }
    throw std::runtime_error("Native link depth exceeds 16 files.");
}
QJsonArray array(const std::vector<double> &values) {
    QJsonArray result;
    for (double value: values) { require(std::isfinite(value), "Non-finite accepted state."); result.append(value); }
    return result;
}
QJsonValue components(const RModel &model, RVariableType type, uint node) {
    const auto index=model.findVariable(type);
    if (index==RConstants::eod) return QJsonValue(QJsonValue::Null);
    const auto &variable=model.getVariable(index);
    if(variable.getNVectors()==0) return QJsonValue(QJsonValue::Null);
    require(variable.getNVectors()==3 && variable.getNValues()==model.getNNodes(), "Unexpected nodal result layout.");
    QJsonArray result;
    for (uint axis=0;axis<3;++axis) {
        const double value=variable.getValue(axis,node);
        require(std::isfinite(value), "Non-finite nodal result."); result.append(value);
    }
    return result;
}
}

int main(int argc,char **argv) {
    QCoreApplication application(argc,argv);
    QCoreApplication::setApplicationName("range-workflow-native");
    QCommandLineParser parser; parser.addHelpOption();
    parser.setApplicationDescription("Versioned native model adapter for Range workflow services.");
    parser.addPositionalArgument("operation", "prepare or inspect");
    parser.addOptions({QCommandLineOption("input","Native model or accepted result.","path"),
        QCommandLineOption("output","Distinct model output path.","path"),
        QCommandLineOption("steps","Number of new or additional steps, 1..5000.","count"),
        QCommandLineOption("dt","Step interval in seconds.","seconds"),
        QCommandLineOption("passive","Disable force control for a new passive baseline."),
        QCommandLineOption("resume","Preserve the accepted checkpoint; steps are additional.")});
    parser.process(application);
    // Native readers log on stdout. Preserve a separate JSON channel.
    std::fflush(stdout);
#ifdef Q_OS_WIN
    const int fd=_dup(_fileno(stdout)); FILE *channel=fd<0?nullptr:_fdopen(fd,"wb");
    const int redirected=_dup2(_fileno(stderr),_fileno(stdout));
#else
    const int fd=dup(fileno(stdout)); FILE *channel=fd<0?nullptr:fdopen(fd,"w");
    const int redirected=dup2(fileno(stderr),fileno(stdout));
#endif
    if (!channel || redirected<0) return 2;
    std::unique_ptr<FILE,decltype(&std::fclose)> guard(channel,&std::fclose);
    auto print=[&](const QJsonObject &value) { const auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact)+'\n';
        std::fwrite(bytes.constData(),1,bytes.size(),channel); std::fflush(channel); };
    try {
        require(parser.positionalArguments().size()==1,"Expected one operation.");
        const auto operation=parser.positionalArguments().first();
        require(operation=="prepare" || operation=="inspect","Unknown native operation.");
        QFileInfo info(parser.value("input"));
        require(info.isFile() && !info.isSymLink(),"Expected regular native input.");
        const QString input=info.canonicalFilePath();
        const auto source=resolveSource(input);
        const auto before=source.files.back().second;
        RModel model; model.read(source.model);
        source.verify();
        auto &restart=model.getProblemSetup().getStructuralDynamicsRestartData();
        if (operation=="inspect") {
            require(!parser.isSet("output") && !parser.isSet("steps") && !parser.isSet("dt")
                    && !parser.isSet("passive") && !parser.isSet("resume"),"Unexpected inspect option.");
            require(model.getNNodes()<=100000,"Nodal query exceeds supported response size.");
            QJsonArray nodes;
            for(uint i=0;i<model.getNNodes();++i) nodes.append(QJsonObject{{"id",int(i)},
                {"displacement",components(model,R_VARIABLE_DISPLACEMENT,i)},
                {"velocity",components(model,R_VARIABLE_VELOCITY,i)},
                {"acceleration",components(model,R_VARIABLE_ACCELERATION,i)},
                {"rotation",components(model,R_VARIABLE_ROTATION,i)},
                {"angularVelocity",components(model,R_VARIABLE_ANGULAR_VELOCITY,i)},
                {"angularAcceleration",components(model,R_VARIABLE_ANGULAR_ACCELERATION,i)}});
            print({{"apiVersion","range.workflow.native/v1"},{"status","ok"},
                {"sourceSha256",QString::fromLatin1(before)},{"nodes",nodes},
                {"sourcePath",source.model},{"sourceFiles",source.references()},
                {"checkpointAvailable",restart.available},{"step",int(restart.completedSteps)},
                {"controlEnabled",model.getProblemSetup().getStructuralControlSetup().enabled},
                {"plannedSteps",int(model.getProblemSetup().getStructuralDynamicsSetup().getNSteps())},
                {"configuredDt",model.getProblemSetup().getStructuralDynamicsSetup().getTimeStep()},
                {"time",restart.time},{"dt",restart.fixedTimeStep},
                {"q",array(restart.q)},{"v",array(restart.v)},{"a",array(restart.a)},
                {"controlAvailable",restart.controlAvailable},
                {"controlWork",array(restart.cumulativeControlWork)}});
            return 0;
        }
        bool valid=false; const auto steps=parser.value("steps").toUInt(&valid);
        require(valid && steps>=1 && steps<=5000,"steps must be an integer in 1..5000.");
        const double dt=parser.value("dt").toDouble(&valid);
        require(valid && std::isfinite(dt) && dt>0.0 && dt<=1.0,"dt must be in (0,1] seconds.");
        const bool resume=parser.isSet("resume");
        require(!resume || (restart.available && restart.fixedTimeStep==dt && !parser.isSet("passive")),
                "Resume requires a compatible checkpoint and unchanged control mode/time step.");
        require(resume || !restart.available,"Use an original model for a new run, or --resume for accepted results.");
        auto &settings=model.getProblemSetup().getStructuralDynamicsSetup();
        settings.setEnabled(true); settings.setTimeStep(dt); settings.setNSteps(steps); settings.setOutputFrequency(steps);
        settings.validate();
        if(parser.isSet("passive")) model.getProblemSetup().getStructuralControlSetup()=RStructuralControlSetup();
        QFileInfo target(parser.value("output"));
        const auto parent=target.dir().canonicalPath();
        require(!parent.isEmpty() && !target.fileName().isEmpty() && !target.isSymLink(),"Invalid output path.");
        const auto output=QDir(parent).filePath(target.fileName());
        require(output.compare(input,Qt::CaseInsensitive)!=0 && !QFileInfo::exists(output),"Output must be a new distinct file.");
        require(target.suffix()=="ram" || target.suffix()=="rbm","Output must be .ram or .rbm.");
        QLockFile lock(output+".lock"); lock.setStaleLockTime(0); require(lock.tryLock(0),"Output is busy.");
        QTemporaryDir temporary(QDir(parent).filePath(".range-workflow-XXXXXX"));
        require(temporary.isValid(),"Cannot stage native model.");
        const auto stagedPath=model.write(temporary.filePath("model."+target.suffix()),false);
        const auto outputHash=digest(stagedPath);
        QFile staged(stagedPath); require(staged.open(QIODevice::ReadOnly),"Cannot read staged model.");
        QSaveFile published(output); require(published.open(QIODevice::WriteOnly),"Cannot stage output.");
        while(!staged.atEnd()) {const auto data=staged.read(1024*1024);
            require(!data.isEmpty() && published.write(data)==data.size(),"Cannot write output.");}
        source.verify();
        require(!QFileInfo::exists(output),"Output changed before publication.");
        require(published.commit(),"Cannot publish output.");
        print({{"apiVersion","range.workflow.native/v1"},{"status","published"},{"output",output},
            {"sourceSha256",QString::fromLatin1(before)},{"sourcePath",source.model},{"sourceFiles",source.references()},
            {"sha256",QString::fromLatin1(outputHash)},
            {"steps",int(steps)},{"dt",dt},{"resume",resume}});
        return 0;
    } catch(const RError &error) {print({{"apiVersion","range.workflow.native/v1"},
        {"status","error"},{"code","NATIVE_ERROR"},{"message",error.getMessage()}});}
      catch(const std::exception &error) {print({{"apiVersion","range.workflow.native/v1"},
        {"status","error"},{"code","NATIVE_ERROR"},{"message",QString::fromUtf8(error.what())}});}
    return 2;
}
