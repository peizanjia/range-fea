#include "piezo_workbench_widget.h"
#include "session.h"
#include "model_command_host.h"
#include "solver_manager.h"
#include "series_plot_widget.h"
#include <rpiezo_experiment_service.h>
#include <rpiezo_artifact_service.h>
#include <piezo_native_backend_bridge.h>
#include <piezo_axial_backend_bridge.h>
#include <piezo_numeric_transport.h>
#include <piezo_spectrum.h>
#include <QCryptographicHash>
#include <rbl_hash_sha256.h>
#include <QApplication>
#include <QCoreApplication>
#include <QDockWidget>
#include <QFile>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QProcess>
#include <QTemporaryFile>
#include <QPointer>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QDir>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMainWindow>
#include <QObject>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <QGuiApplication>
#include "model_tree.h"
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <rml_file_io.h>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#include <algorithm>
#include <functional>
#include <QColor>
#include <QFont>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
class ResponsiveResultFieldGrid final : public QObject {
public:
    ResponsiveResultFieldGrid(QWidget *host,QGridLayout *grid,QWidget *port,QWidget *quantity,
                               QWidget *reference,QWidget *view,int wideThreshold)
        : QObject(host),host_(host),grid_(grid),fields_{port,quantity,reference,view},wideThreshold_(wideThreshold) {
        host_->installEventFilter(this);
        applyMode();
    }

protected:
    bool eventFilter(QObject *watched,QEvent *event) override {
        if(watched==host_&&event->type()==QEvent::Resize)applyMode();
        return QObject::eventFilter(watched,event);
    }

private:
    void applyMode() {
        const bool wide=host_->contentsRect().width()>=wideThreshold_;
        if(mode_==int(wide))return;
        mode_=int(wide);
        grid_->setColumnStretch(0,1);
        grid_->setColumnStretch(1,wide?1:0);
        for(auto *field:fields_)grid_->removeWidget(field);
        static constexpr int widePositions[4][2]{{0,0},{0,1},{1,0},{1,1}};
        for(int i=0;i<4;++i)grid_->addWidget(fields_[i],wide?widePositions[i][0]:i,wide?widePositions[i][1]:0);
    }

    QWidget *host_ = nullptr;
    QGridLayout *grid_ = nullptr;
    QWidget *fields_[4]{};
    int wideThreshold_ = 0;
    int mode_ = -1;
};

int resultFieldInlineWidth(QFormLayout *form,QWidget *field) {
    const auto *label=qobject_cast<QLabel*>(form->labelForField(field));
    int left=0,top=0,right=0,bottom=0;form->getContentsMargins(&left,&top,&right,&bottom);
    const int spacing=form->horizontalSpacing()>=0?form->horizontalSpacing():form->spacing();
    return (label?label->sizeHint().width():0)+field->minimumSizeHint().width()+std::max(0,spacing)+left+right;
}

void updateSpectralResultRows(QFormLayout *form,QComboBox *view,QComboBox *window,QCheckBox *removeMean) {
    if(!form||!view)return;
    const bool spectral=view->currentData().toString()!="time";
    form->setRowVisible(window,spectral);
    form->setRowVisible(removeMean,spectral);
}

class ScientificDoubleSpinBox final : public QDoubleSpinBox {
public:
    explicit ScientificDoubleSpinBox(QWidget *parent) : QDoubleSpinBox(parent) {setDecimals(323);}
protected:
    QString textFromValue(double value) const override {return QString::number(value,'g',17);}
    double valueFromText(const QString &text) const override {
        bool ok=false;const double parsed=QLocale::c().toDouble(text.trimmed(),&ok);
        return ok&&std::isfinite(parsed)?parsed:value();
    }
    QValidator::State validate(QString &text,int &position) const override {
        QDoubleValidator validator(minimum(),maximum(),323);
        auto locale=QLocale::c();locale.setNumberOptions(QLocale::RejectGroupSeparator);validator.setLocale(locale);
        validator.setNotation(QDoubleValidator::ScientificNotation);
        const auto state=validator.validate(text,position);bool ok=false;const double parsed=locale.toDouble(text,&ok);
        return state==QValidator::Acceptable&&(!ok||!std::isfinite(parsed))?QValidator::Invalid:state;
    }
    void stepBy(int steps) override {
        const double increment=value()==0?singleStep():std::max(std::abs(value())*.1,std::numeric_limits<double>::denorm_min());
        const double next=value()+double(steps)*increment;
        if(std::isfinite(next))setValue(std::clamp(next,minimum(),maximum()));
    }
};
QString readoutModeName(int mode) {
    const QStringList modes{QObject::tr("Voltage source action"),QObject::tr("Open-circuit voltage"),QObject::tr("Short-circuit charge"),QObject::tr("Ideal charge amplifier"),QObject::tr("Actuator charge")};
    return mode>=0&&mode<modes.size()?modes[mode]:QObject::tr("Unknown readout");
}
QString sourceFingerprint(const RModel &model) {
    if(model.getProblemSetup().getStructuralPiezoAxialSetup().enabled){const auto &s=model.getProblemSetup().getStructuralPiezoAxialSetup();s.validate(model);return s.computeSourceFingerprint(model);}
    const auto &s=model.getProblemSetup().getStructuralPiezoLayeredSetup();s.validate(model);return s.computeSourceFingerprint(model);
}
QString token() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool targetsSelectedDocument(Session *session,const QString &path) {
    if(!session || !session->trylock())return true;
    struct Unlock{Session *session;~Unlock(){session->unlock();}} unlock{session};
    const auto ids=session->getSelectedModelIDs();if(ids.size()!=1)return true;
    const auto source=session->getModel(ids.first()).getFileName();if(source.isEmpty())return false;
    const auto canonical=[](const QString &value){const QFileInfo file(value);const auto resolved=file.canonicalFilePath();return resolved.isEmpty()?file.absoluteFilePath():resolved;};
    return canonical(path).compare(canonical(source),Qt::CaseInsensitive)==0;
}
QJsonObject request(const QString &operation, const QJsonObject &parameters = {}) {
    return {{"apiVersion","range.host/v1"},{"requestId",token()},
            {"operation",operation},{"parameters",parameters}};
}
QJsonValue replaceAt(QJsonValue current, const QStringList &path, int offset, const QJsonValue &value) {
    if (offset == path.size()) return value;
    if (current.isArray()) {
        auto array = current.toArray();
        bool ok = false; int index = path[offset].toInt(&ok);
        if (!ok || index < 0 || index >= array.size()) return current;
        array[index] = replaceAt(array[index],path,offset+1,value); return array;
    }
    auto object = current.toObject();
    object[path[offset]] = replaceAt(object[path[offset]],path,offset+1,value); return object;
}
QJsonArray zeros(int count, double value = 0) { QJsonArray a; for(int i=0;i<count;++i) a.append(value); return a; }
QJsonValue remapNodeBindings(QJsonValue value,const QJsonObject &mapping) {
    if(value.isArray()){auto array=value.toArray();for(int i=0;i<array.size();++i)array[i]=remapNodeBindings(array[i],mapping);return array;}
    if(!value.isObject())return value;auto object=value.toObject();
    for(auto it=object.begin();it!=object.end();++it){
        if(it.key()=="physicalNodeId"){const auto key=QString::number(it.value().toDouble(),'f',0);if(mapping.contains(key))it.value()=mapping[key];}
        else if(it.key()=="channelIds"&&it.value().isArray()){auto ids=it.value().toArray();for(int i=0;i<ids.size();++i){auto parts=ids[i].toString().split(':');if((parts.size()==3||parts.size()==4)&&parts[0]=="node"&&mapping.contains(parts[1]))ids[i]=QString("node:%1:%2").arg(mapping[parts[1]].toDouble(),0,'f',0).arg(parts.mid(2).join(':'));}it.value()=ids;}
        else if(it.key()=="channelId"){auto parts=it.value().toString().split(':');if((parts.size()==3||parts.size()==4)&&parts[0]=="node"&&mapping.contains(parts[1]))it.value()=QString("node:%1:%2").arg(mapping[parts[1]].toDouble(),0,'f',0).arg(parts.mid(2).join(':'));}
        else it.value()=remapNodeBindings(it.value(),mapping);
    }return object;
}
QJsonArray diagonal(int rows, int columns, double value = 0) {
    QJsonArray a; for(int i=0;i<rows;++i) { auto row=zeros(columns); if(i<columns) row[i]=value; a.append(row); } return a;
}
QLabel *note(const QString &text, QWidget *parent) { auto *l=new QLabel(text,parent); l->setWordWrap(true); return l; }
QTableWidget *table(const QStringList &columns, QWidget *parent, const QString &name) {
    auto *t=new QTableWidget(0,columns.size(),parent); t->setObjectName(name);
    t->setHorizontalHeaderLabels(columns); t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->setMinimumHeight(130); return t;
}
int previewContentHeight(const QTableWidget *preview) {
    int height=preview->horizontalHeader()->height()+2*preview->frameWidth();
    for(int row=0;row<preview->rowCount();++row)height+=preview->rowHeight(row);
    return height;
}
QString nodeLabel(int index, const RNode &n) {
    return QObject::tr("Node %1 (%2, %3, %4 m)").arg(index+1)
        .arg(n.getX(),0,'g',5).arg(n.getY(),0,'g',5).arg(n.getZ(),0,'g',5);
}
QString rowIndex(int i) {return QString::number(i);}
QString forceDirection(const QString &channelId) {return channelId.split(':').mid(2).join(':');}
QString forceUnit(const QString &direction) {return direction.section(':',-1).startsWith('R')?"N m":"N";}
QString forceChannelId(double node,const QString &direction) {return QString("node:%1:%2").arg(node,0,'f',0).arg(direction);}
QString invalidRecipeValuePath(const QJsonValue &value,const QString &path={}) {
    if(value.isNull()||value.isUndefined()||(value.isDouble()&&!std::isfinite(value.toDouble())))return path.isEmpty()?QString("/"):path;
    if(value.isArray()){const auto values=value.toArray();for(int i=0;i<values.size();++i){const auto bad=invalidRecipeValuePath(values[i],path+"/"+QString::number(i));if(!bad.isEmpty())return bad;}}
    if(value.isObject()){const auto values=value.toObject();for(auto it=values.constBegin();it!=values.constEnd();++it){auto key=it.key();key.replace("~","~0").replace("/","~1");const auto bad=invalidRecipeValuePath(it.value(),path+"/"+key);if(!bad.isEmpty())return bad;}}
    return {};
}
QVector<QJsonObject> externalProgramConnections(const QJsonObject &recipe) {
    QVector<QJsonObject> result;auto c=recipe.value("controller").toObject();if(c.value("kind")==QJsonValue("external"))result.append(c.value("external").toObject().value("connection").toObject());
    for(const auto &v:recipe.value("excitation").toObject().value("externalSources").toArray())result.append(v.toObject().value("connection").toObject());return result;
}
QString fileDigest(const QString &path) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("External force source or dependency is unavailable");
    RHashSha256 hash;
    if(!hash.addData(&file)||file.error()!=QFileDevice::NoError)throw std::runtime_error("External force source or dependency cannot be read");
    return QString::fromLatin1(hash.result().toHex());
}
#ifdef Q_OS_WIN
QString nativeFileChangeTime(const QString &path) {
    QString nativePath=QDir::toNativeSeparators(path);
    if(!nativePath.startsWith(QStringLiteral("\\\\?\\"))){
        if(nativePath.startsWith(QStringLiteral("\\\\")))nativePath=QStringLiteral("\\\\?\\UNC\\")+nativePath.mid(2);
        else nativePath=QStringLiteral("\\\\?\\")+nativePath;
    }
    const HANDLE handle=CreateFileW(reinterpret_cast<LPCWSTR>(nativePath.utf16()),FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle==INVALID_HANDLE_VALUE){
        if(qEnvironmentVariableIsSet("RANGE_PIEZO_WATCH_DIAGNOSTICS"))qWarning().noquote()<<"Native file change-time open failed"<<GetLastError()<<path;
        return {};
    }
    FILE_BASIC_INFO basic{};
    const BOOL read=GetFileInformationByHandleEx(handle,FileBasicInfo,&basic,sizeof(basic));
    const DWORD readError=read?ERROR_SUCCESS:GetLastError();
    CloseHandle(handle);
    if(!read&&qEnvironmentVariableIsSet("RANGE_PIEZO_WATCH_DIAGNOSTICS"))qWarning().noquote()<<"Native file change-time query failed"<<readError<<path;
    return read?QString::number(qulonglong(basic.ChangeTime.QuadPart)):QString{};
}
#endif
QJsonObject acceptedFileStamp(const QString &path) {
    const QFileInfo info(path);
    const auto modified=info.lastModified();
    if(!info.isFile()||info.isSymLink()||info.canonicalFilePath().isEmpty()||!modified.isValid())return {};
    QJsonObject stamp{{"canonicalPath",info.canonicalFilePath()},
                      {"size",QString::number(info.size())},
                      {"modifiedMs",QString::number(modified.toMSecsSinceEpoch())}};
#ifdef Q_OS_WIN
    const auto changeTime=nativeFileChangeTime(info.canonicalFilePath());
    if(changeTime.isEmpty())return {};
    stamp.insert("changeTimeTicks",changeTime);
#endif
    return stamp;
}
void requireResult(bool condition,const QString &message) {if(!condition)throw std::runtime_error(message.toUtf8().constData());}
void safeResultPath(const QString &path) {
    QFileInfo info(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    for(;;){requireResult(!info.isSymLink(),QObject::tr("Accepted history paths cannot contain symbolic links."));const QString parent=info.absolutePath();if(parent==info.absoluteFilePath())break;info=QFileInfo(parent);}
}
QByteArray resultBytes(const QString &path,qint64 maximum) {
    safeResultPath(path);QFileInfo info(path);requireResult(info.isFile()&&info.size()<=maximum,QObject::tr("Accepted history authority file is unavailable or exceeds its bound."));
    QFile file(path);requireResult(file.open(QIODevice::ReadOnly),QObject::tr("Cannot read accepted history authority."));const auto bytes=file.readAll();requireResult(file.error()==QFileDevice::NoError,QObject::tr("Cannot read accepted history authority."));return bytes;
}
QJsonObject resultObject(const QByteArray &bytes) {
    QJsonParseError error;const auto document=QJsonDocument::fromJson(bytes,&error);requireResult(error.error==QJsonParseError::NoError&&document.isObject(),QObject::tr("Accepted history authority is invalid JSON."));return document.object();
}
void resultKeys(const QJsonObject &object,QStringList expected) {auto actual=object.keys();actual.sort();expected.sort();requireResult(actual==expected,QObject::tr("Accepted history authority fields differ from the supported schema."));}
QByteArray resultSha(const QJsonValue &value) {
    const auto text=value.toString();requireResult(text.size()==64,QObject::tr("Accepted history SHA-256 is invalid."));for(const auto c:text)requireResult((c>='0'&&c<='9')||(c>='a'&&c<='f'),QObject::tr("Accepted history SHA-256 is invalid."));return QByteArray::fromHex(text.toLatin1());
}
quint64 resultInteger(const QJsonValue &value,quint64 maximum) {const double x=value.toDouble(-1);requireResult(value.isDouble()&&std::isfinite(x)&&x>=0&&std::floor(x)==x&&x<=double(maximum),QObject::tr("Accepted history integer is invalid."));return quint64(x);}
bool sameResultPath(const QString &a,const QString &b) {return QDir::cleanPath(QFileInfo(a).absoluteFilePath()).compare(QDir::cleanPath(QFileInfo(b).absoluteFilePath()),Qt::CaseInsensitive)==0;}
QString resultBasename(const QJsonValue &value) {const auto name=value.toString();requireResult(!name.isEmpty()&&name!="."&&name!=".."&&!name.contains('/')&&!name.contains('\\')&&!name.contains(':'),QObject::tr("Accepted history basename is invalid."));return name;}
QStringList namedResultChannels(const QJsonObject &recipe) {
    QStringList ids;for(const auto &value:recipe.value("ports").toArray()){const auto port=value.toObject();const QString base="piezo:port:"+QString::fromLatin1(QUrl::toPercentEncoding(port.value("portId").toString()))+":";
        for(const auto &quantity:QStringList{"readout","voltage","charge"})for(const auto &reference:QStringList{"absolute","delta"})ids.append(base+quantity+":"+reference);
        if(port.value("readout")=="idealChargeAmplifier")for(const auto &reference:QStringList{"absolute","delta"})ids.append(base+"amplifierVoltage:"+reference);
    }requireResult(!ids.isEmpty(),QObject::tr("The saved experiment has no named electrode catalog."));return ids;
}
void rawResultModel(const QString &path) {
    const auto suffix=QFileInfo(path).suffix().toLower();requireResult(suffix=="ram"||suffix=="rbm",QObject::tr("Original accepted history input must be a raw native MODEL."));
    RFile file(path,suffix=="ram"?RFile::ASCII:RFile::BINARY);requireResult(file.open(suffix=="ram"?(QIODevice::ReadOnly|QIODevice::Text):QIODevice::ReadOnly),QObject::tr("Original accepted history input cannot be read."));RFileHeader header;
    if(suffix=="ram")RFileIO::readAscii(file,header);else RFileIO::readBinary(file,header);requireResult(header.getType()==R_FILE_TYPE_MODEL,QObject::tr("Original accepted history input cannot be a LINK."));
}
QJsonObject freezeFunction(QJsonObject function,const QStringList &dependencies) {
    const QFileInfo source(function.value("sourcePath").toString());
    if(!source.isFile()||source.suffix().compare("py",Qt::CaseInsensitive)!=0)throw std::runtime_error("Select an existing Python force function file");
    function["sourcePath"]=source.canonicalFilePath();function["sourceSha256"]=fileDigest(source.canonicalFilePath());
    QJsonArray files;std::set<QString> seen;
    for(const auto &path:dependencies){const QFileInfo info(path);const auto absolute=info.canonicalFilePath();
        if(!info.isFile()||absolute.isEmpty())throw std::runtime_error("External force dependency is unavailable");
        if(seen.insert(absolute).second)files.append(QJsonObject{{"path",absolute},{"sha256",fileDigest(absolute)}});
    }function["dependencyFiles"]=files;
    const auto runtime=PiezoNumericTransport::defaultRuntime();PiezoNumericTransport transport(runtime.first,runtime.second);
    const auto metadata=transport.runtimeMetadata();
    function["environment"]=QJsonObject{{"pythonVersion",metadata.value("python")},{"numpyVersion",metadata.value("numpy")},{"scipyVersion",metadata.value("scipy")}};
    return function;
}
QStringList functionDependencies(const QJsonObject &function) {
    QStringList paths;for(const auto &value:function.value("dependencyFiles").toArray())paths.append(value.toObject()["path"].toString());return paths;
}
bool sourceSnapshot(Session *session, RModel *model, bool *sourceBusy=nullptr) {
    if(sourceBusy)*sourceBusy=false;
    if(!session)return false;
    if(!session->trylock()){if(sourceBusy)*sourceBusy=true;return false;}
    struct Unlock {Session *session;~Unlock(){session->unlock();}} unlock{session};
    const auto ids=session->getSelectedModelIDs();
    if(ids.size()!=1)return false;
    *model=session->getModel(ids.first());return true;
}
QJsonArray feedbackCatalog(const QJsonObject &recipe) {
    QJsonArray outputs;
    for(const auto &value:recipe.value("ports").toArray()){
        auto p=value.toObject();const auto readout=p.value("readout").toString();
        if(readout=="none")continue;
        outputs.append(QJsonObject{{"id",p.value("portId")},{"unit",readout=="openCircuitVoltage"||readout=="idealChargeAmplifier"?"V":"C"},{"quantity","piezo_readout"}});
    }
    for(const auto &value:recipe.value("design").toObject()["outputs"].toArray())outputs.append(value);
    return outputs;
}
QJsonObject modalInputRecipe(QJsonObject recipe) {
    recipe["controller"]=QJsonObject{{"kind","none"}};
    recipe["damping"]=QJsonObject{{"kind","none"}};
    auto design=recipe.value("design").toObject();design.remove("modeIndices");recipe["design"]=design;
    recipe["replaceLegacyConditions"]=true;return recipe;
}
QJsonObject templateRecipe(const RModel &source,const QJsonObject &t) {
    const bool plate=t.value("kind")==QJsonValue("flatPlate");
    const bool rod=t.value("kind")==QJsonValue("axialTruss");
    auto parameters=t;if(!t.value("kind").toString().startsWith("solid"))parameters.remove("patchThickness");
    const auto generated=RPiezoExperimentService::templateCandidate(source,parameters);
    const bool solid=t.value("kind").toString().startsWith("solid");
    auto recipe=RPiezoExperimentService::exampleRecipe(generated,solid?"solid3D":rod?"axialRod":plate?"flatPlate":"planarBeam");
    if(recipe.isEmpty())throw std::runtime_error("Unsupported generated template");
    if(solid){recipe["template"]=parameters;return recipe;}
    if(rod){auto members=recipe.value("members").toArray();for(int i=0;i<members.size();++i){auto member=members[i].toObject();member["area"]=t.value("width").toDouble()*t.value("thickness").toDouble();members[i]=member;}recipe["members"]=members;recipe["template"]=parameters;return recipe;}
    auto members=recipe.value("members").toArray();const double width=plate?1.:t.value("width").toDouble(),half=t.value("thickness").toDouble()/2;
    for(int i=0;i<members.size();++i){auto member=members[i].toObject();auto layers=member.value("layers").toArray();
        for(int j=0;j<layers.size();++j){auto layer=layers[j].toObject();layer["width"]=width;
            if(j==0){layer["lowerZ"]=-half;layer["upperZ"]=half;}
            else if(j==1){layer["lowerZ"]=half;layer["upperZ"]=half+.0005;}
            else{layer["lowerZ"]=-half-.0005;layer["upperZ"]=-half;}
            layers[j]=layer;
        }member["layers"]=layers;members[i]=member;
    }
    recipe["members"]=members;recipe["template"]=parameters;return recipe;
}
QString piezoEditorObjectName(const QStringList &path,const QString &requestedName={}) {
    if(!requestedName.isEmpty())return requestedName;
    QString name="piezoEditor";
    for(const auto &part:path){name+='_';for(const auto character:part)name+=character.isLetterOrNumber()||character=='_'?character:QChar('_');}
    return name;
}
QString namedFocusWithin(QWidget *focus,const QWidget *root) {
    QString name;
    while(focus&&focus!=root){
        if(name.isEmpty()&&!focus->objectName().isEmpty()&&!focus->objectName().startsWith("qt_"))name=focus->objectName();
        focus=focus->parentWidget();
    }
    return focus==root?name:QString();
}
QString piezoEditorAccessibleName(const QStringList &path) {
    if(path.isEmpty())return QCoreApplication::translate("PiezoWorkbenchWidget","Experiment setting");
    QString key=path.last();
    if(key.toInt()!=0||key=="0")key=path.size()>1?path[path.size()-2]:QStringLiteral("value");
    static const QHash<QString,QString> labels{
        {"kind","Type"},{"physicalNodeId","Bound node"},{"materialId","Material"},
        {"area","Cross-section area (m²)"},{"portId","Electrode port"},{"initialTension","Initial cable tension (N)"},
        {"width","Width (m)"},{"polarity","Polarity"},{"coverage","Patch coverage coordinate"},
        {"form","Material data form"},{"density","Density (kg/m³)"},{"sourceKind","Material source"},
        {"role","Port role"},{"readout","Electrical readout"},{"Cf","Feedback capacitance (F)"},
        {"initialValue","Initial readout value"},{"Qref","Reference charge (C)"},{"VoutBias","Output bias (V)"},
        {"value","Value"},{"values","Nodal component value"},{"q0","Reference displacement component"},
        {"f0","Reference force component"},{"V0","Initial electrode voltage (V)"},{"Q0","Initial electrode charge (C)"},
        {"quantity","Quantity"},{"dof","Degree of freedom"},{"dt","FE time step (s)"},{"steps","FE steps"},
        {"uniformRatio","Uniform damping ratio"},{"rayleighAlpha","Rayleigh alpha (1/s)"},{"rayleighBeta","Rayleigh beta (s)"},
        {"ratios","Damping ratio"},{"importedMatrix","Imported damping matrix C"},
        {"ex","Local X axis components"},{"ey","Local Y axis components"},{"ez","Local Z axis components"}
    };
    QString label=labels.value(key);
    if(label.isEmpty()){
        label=key;for(int i=label.size()-1;i>0;--i)if(label[i].isUpper()&&label[i-1].isLower())label.insert(i,' ');
        label.replace('_',' ');if(!label.isEmpty())label[0]=label[0].toUpper();
    }
    const QString context=path.first();
    if(context=="controller"&&key!="kind")label=QCoreApplication::translate("PiezoWorkbenchWidget","Controller %1").arg(label);
    else if(context=="template"&&key=="kind")label=QCoreApplication::translate("PiezoWorkbenchWidget","Template type");
    else if(context=="template")label=QCoreApplication::translate("PiezoWorkbenchWidget","Template %1").arg(label);
    else if(context=="members"&&key=="kind")label=QCoreApplication::translate("PiezoWorkbenchWidget","Device kind");
    else if(context=="damping"&&key=="kind")label=QCoreApplication::translate("PiezoWorkbenchWidget","Physical damping authority");
    else if(context=="workpoint"&&key=="V0")label=QCoreApplication::translate("PiezoWorkbenchWidget","Initial electrode voltage (V)");
    return QCoreApplication::translate("PiezoWorkbenchWidget",label.toUtf8().constData());
}
}

void PiezoWorkbenchWidget::invalidatePerformance(const QString &reason) {
    performanceInvalid=true;performancePendingPaint=false;setProperty("piezoMeasurementInvalid",true);setProperty("piezoMeasurementError",reason);
    qCritical().noquote()<<"RANGE_PIEZO_MEASUREMENT_INVALID"<<performanceRequestId<<reason;
}
void PiezoWorkbenchWidget::performanceEvent(const QString &event,QJsonObject fields) {
    if(!performanceEnabled||performanceInvalid)return;
    try{
        safeResultPath(performanceWorkspace);safeResultPath(performanceEvents);const QFileInfo root(performanceWorkspace),target(performanceEvents);
        requireResult(root.isDir()&&!root.canonicalFilePath().isEmpty()&&QFileInfo(target.absolutePath()).isDir(),tr("Performance evidence directory is unavailable."));
        const auto relative=QDir(root.canonicalFilePath()).relativeFilePath(QFileInfo(target.absolutePath()).canonicalFilePath()+"/"+target.fileName());
        requireResult(!QDir::isAbsolutePath(relative)&&relative!=".."&&!relative.startsWith("../"),tr("Performance evidence path escaped its explicit workspace."));
        requireResult(!target.exists()||target.isFile(),tr("Performance evidence path is not a regular file."));
        for(auto it=performanceBinding.constBegin();it!=performanceBinding.constEnd();++it)if(!fields.contains(it.key()))fields[it.key()]=it.value();
        fields["schema"]="range.piezo-gui-performance-event/v1";fields["requestRoute"]=performanceOrigin;fields["guiExecutable"]=QCoreApplication::applicationFilePath();fields["requestId"]=performanceRequestId;fields["operation"]=performanceOperation;fields["event"]=event;fields["elapsedNanoseconds"]=QString::number(performanceTimer.nsecsElapsed());fields["utc"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);fields["guiPid"]=QString::number(QCoreApplication::applicationPid());fields["measurementValid"]=true;
        const auto bytes=QJsonDocument(fields).toJson(QJsonDocument::Compact)+'\n';constexpr qint64 maximum=16LL*1024*1024;
        requireResult(bytes.size()<=64*1024&&target.size()<=maximum-bytes.size(),tr("Performance evidence exceeded its bounded log size."));
        QFile file(performanceEvents);requireResult(file.open(QIODevice::WriteOnly|QIODevice::Append)&&file.size()<=maximum-bytes.size()&&file.write(bytes)==bytes.size()&&file.flush()&&file.error()==QFileDevice::NoError,tr("Performance evidence event could not be written completely."));
    }catch(const std::exception &error){invalidatePerformance(QString::fromUtf8(error.what()));}catch(...){invalidatePerformance(tr("Performance evidence logging failed."));}
}
void PiezoWorkbenchWidget::beginPerformanceRequest(const QString &operation,const QJsonObject &arguments) {
    if(performancePendingPaint)performanceEvent("request_superseded");performanceTimer.start();performanceRequestId=token();performanceOperation=operation;performanceOrigin=arguments.value("origin").toString("product-native-path");performanceGeneration=0;performancePendingPaint=false;performanceSource.clear();performanceArtifact.clear();performanceChannel.clear();performanceBinding={};
    performanceWorkspace=qEnvironmentVariable("RANGE_PIEZO_PERFORMANCE_WORKSPACE");performanceEvents=qEnvironmentVariable("RANGE_PIEZO_PERFORMANCE_EVENTS");performanceEnabled=!performanceWorkspace.isEmpty()||!performanceEvents.isEmpty();
    if(!performanceEnabled)return;
    if(performanceWorkspace.isEmpty()||performanceEvents.isEmpty()||!QDir::isAbsolutePath(performanceWorkspace)||!QDir::isAbsolutePath(performanceEvents)){invalidatePerformance(tr("Performance evidence requires absolute workspace and NDJSON paths."));return;}
    performanceEvent("request_begin",{{"arguments",arguments}});
}
QJsonObject PiezoWorkbenchWidget::acceptedPerformanceIdentity() const {
    QJsonObject identity;const auto bytes=QJsonDocument(acceptedSeriesDocument).toJson(QJsonDocument::Compact);identity["sourceBindingBytesBase64"]=QString::fromLatin1(bytes.toBase64());identity["sourceBindingSha256"]=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
    if(acceptedNumericSeries){const auto &d=*acceptedNumericSeries;identity["storage"]="compact-accepted-trace";identity["family"]=d.family;identity["modelFamily"]=d.modelFamily;identity["sourceFingerprint"]=d.sourceFingerprint;identity["configurationIdentity"]=d.configurationIdentity;identity["backendIdentity"]=d.backendIdentity;identity["artifactSha256"]=d.artifactSha256;identity["totalRecords"]=double(d.times.size());identity["baseSeq"]=double(d.baseSeq);identity["targetSteps"]=double(d.targetSteps);identity["initialTimeS"]=d.initialTimeS;identity["dtS"]=d.dtS;identity["firstFE"]=double(d.completedFE.first());identity["lastFE"]=double(d.completedFE.last());identity["firstTimeS"]=d.times.first();identity["lastTimeS"]=d.times.last();identity["coverage"]=d.coverage;
        identity["baseModelSha256"]=QString::fromLatin1(acceptedTraceProof.baseModelSha256.toHex());identity["headerSha256"]=QString::fromLatin1(acceptedTraceProof.expectedHeaderSha256.toHex());identity["contextSha256"]=QString::fromLatin1(acceptedTraceProof.expectedContextSha256.toHex());identity["committedByteEnd"]=QString::number(acceptedTraceProof.cursor.byteEnd);identity["terminalRecordSha256"]=QString::fromLatin1(acceptedTraceProof.cursor.recordSha32.toHex());identity["terminalStateSha256"]=QString::fromLatin1(acceptedTraceProof.cursor.stateSha32.toHex());
    }else if(acceptedSeries&&!acceptedSeries->frames.isEmpty()){const auto &d=*acceptedSeries;identity["storage"]="selected-raw-model-records";identity["family"]=d.family;identity["modelFamily"]=d.modelFamily;identity["sourceFingerprint"]=d.sourceFingerprint;identity["configurationIdentity"]=d.configurationIdentity;identity["backendIdentity"]=d.backendIdentity;identity["artifactSha256"]=d.artifactSha256;identity["totalRecords"]=double(d.frames.size());identity["firstFE"]=double(d.frames.first()->completedFE);identity["lastFE"]=double(d.frames.last()->completedFE);identity["targetSteps"]=double(d.frames.first()->targetSteps);identity["dtS"]=d.frames.first()->dtS;identity["coverage"]=d.coverage;}
    return identity;
}
void PiezoWorkbenchWidget::bindPerformanceTrace(const PiezoResultSeries::TraceProof &proof,const QJsonObject &document) {
    if(!performanceEnabled||performanceInvalid)return;
    const auto bytes=QJsonDocument(document).toJson(QJsonDocument::Compact);performanceBinding={{"sourceBindingBytesBase64",QString::fromLatin1(bytes.toBase64())},{"sourceBindingSha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())},{"sourceFingerprint",proof.expectedSourceFingerprint},{"baseModelSha256",QString::fromLatin1(proof.baseModelSha256.toHex())},{"headerSha256",QString::fromLatin1(proof.expectedHeaderSha256.toHex())},{"contextSha256",QString::fromLatin1(proof.expectedContextSha256.toHex())},{"committedByteEnd",QString::number(proof.cursor.byteEnd)},{"terminalRecordSha256",QString::fromLatin1(proof.cursor.recordSha32.toHex())},{"terminalStateSha256",QString::fromLatin1(proof.cursor.stateSha32.toHex())},{"acceptedSeq",QString::number(proof.cursor.sequence)}};
    try{QFile file(proof.tracePath);requireResult(file.open(QIODevice::ReadOnly),tr("Cannot inspect performance trace clock."));const auto header=PiezoAcceptedTrace::readHeader(file);performanceBinding["family"]=header.family;performanceBinding["configurationIdentity"]=header.configurationIdentity;performanceBinding["backendIdentity"]=header.backendIdentity;performanceBinding["baseSeq"]=QString::number(header.baseSeq);performanceBinding["targetSteps"]=QString::number(header.targetSteps);performanceBinding["initialTimeS"]=header.initialTime;performanceBinding["dtS"]=header.dt;performanceBinding["targetTimeGridSha256"]=QString::fromLatin1(header.targetTimeGridSha256.toHex());}
    catch(const std::exception &error){invalidatePerformance(QString::fromUtf8(error.what()));}catch(...){invalidatePerformance(tr("Performance trace observation failed."));}
}
void PiezoWorkbenchWidget::bindPerformancePlot() {
    if(!performanceEnabled||performanceInvalid||performanceOperation!="plot"||!resultPlot)return;
    if(performanceGeneration){if(performancePendingPaint)invalidatePerformance(tr("The selected dataset changed before its measured paint."));return;}
    const auto &plot=resultPlot->dataset();if(plot.traces.size()!=1||plot.traces.first().points.isEmpty())return;
    const auto &trace=plot.traces.first();performanceGeneration=resultPlot->datasetGeneration();performanceSource=trace.sourceId;performanceArtifact=trace.artifactSha256;performanceChannel=trace.id;performancePendingPaint=true;performanceBinding=acceptedPerformanceIdentity();
    performanceBinding["channelId"]=trace.id;performanceBinding["unit"]=trace.unit;performanceBinding["view"]=resultViewKind;performanceBinding["datasetGeneration"]=QString::number(performanceGeneration);performanceEvent("dataset_ready",performanceBinding);
}
void PiezoWorkbenchWidget::acceptedDatasetPainted(quint64 generation) {
    if(!performancePendingPaint||!performanceEnabled||performanceInvalid||generation!=performanceGeneration||!resultPlot)return;
    const auto &plot=resultPlot->dataset();if(plot.traces.size()!=1||plot.traces.first().id!=performanceChannel||plot.traces.first().sourceId!=performanceSource||plot.traces.first().artifactSha256!=performanceArtifact||resultPlot->datasetGeneration()!=generation){invalidatePerformance(tr("Painted dataset differs from the measured accepted trace."));return;}
    const auto *canvas=resultPlot->findChild<QWidget*>("seriesPlotCanvas");const auto *nativeWindow=window()->windowHandle();auto fields=performanceBinding;const auto &trace=plot.traces.first();fields["displayRecords"]=double(trace.points.size());fields["firstDisplayedOrdinal"]=QString::number(trace.originalOrdinals.first());fields["lastDisplayedOrdinal"]=QString::number(trace.originalOrdinals.last());fields["platform"]=QGuiApplication::platformName();fields["windowVisible"]=window()->isVisible();fields["canvasVisible"]=canvas&&canvas->isVisible();fields["windowExposed"]=nativeWindow&&nativeWindow->isExposed();fields["windowId"]=QString::number(quintptr(window()->winId()));fields["canvasWidth"]=canvas?canvas->width():0;fields["canvasHeight"]=canvas?canvas->height():0;
    fields["installedWindowQualified"]=performanceOrigin=="product-native-path"&&QFileInfo(QCoreApplication::applicationFilePath()).fileName().compare("fea.exe",Qt::CaseInsensitive)==0&&QGuiApplication::platformName()=="windows"&&window()->isVisible()&&canvas&&canvas->isVisible()&&nativeWindow&&nativeWindow->isExposed();performancePendingPaint=false;performanceEvent("canvas_painted",fields);
}

PiezoWorkbenchWidget::PiezoWorkbenchWidget(QWidget *parent) : QWidget(parent) {
    setObjectName("piezoWorkbench");
    auto *layout=new QVBoxLayout(this);
    source=note(tr("Select one native document to configure its experiment."),this);
    source->setObjectName("piezoSource");source->setMinimumWidth(0);
    source->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
    auto *sourceRow=new QHBoxLayout;sourceRow->setContentsMargins(0,0,0,0);
    sourceRow->addWidget(source,1);
    auto *chooseModelButton=new QPushButton(tr("Choose model"),this);
    chooseModelButton->setObjectName("piezoChooseModel");
    chooseModelButton->setAccessibleName(tr("Choose model"));
    chooseModelButton->setAccessibleDescription(tr("Show and focus the model tree so you can select exactly one model."));
    chooseModelButton->setToolTip(chooseModelButton->accessibleDescription());
    chooseModelButton->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);
    chooseModelButton->hide();sourceRow->addWidget(chooseModelButton);layout->addLayout(sourceRow);
    connect(chooseModelButton,&QPushButton::clicked,this,[this]{
        auto *mainWindow=qobject_cast<QMainWindow*>(window());
        auto *modelDock=mainWindow?mainWindow->findChild<QDockWidget*>("modelDock"):nullptr;
        if(!modelDock){status->setText(tr("The Models panel is unavailable. Select exactly one model in the model tree."));return;}
        modelDock->show();modelDock->raise();
        if(auto *modelTabs=qobject_cast<QTabWidget*>(modelDock->widget()))
            if(auto *modelPage=modelDock->findChild<QScrollArea*>("modelTabScroll"))modelTabs->setCurrentWidget(modelPage);
        if(auto *modelTree=modelDock->findChild<ModelTree*>())modelTree->setFocus(Qt::OtherFocusReason);
        else modelDock->setFocus(Qt::OtherFocusReason);
    });
    layout->addWidget(note(tr("Axial rods, planar beams and flat Q4 plates with ideal bonded patches."),this));
    auto *scopeDetails=note(tr("Three-dimensional axial rods, planar axial/bending layered beams and flat Q4 plates; ideal bonded patches. "
        "The complete replacement includes actual mass, stiffness, electrodes and readout. "
        "Torsion, the other beam bending plane and curved shells are unsupported. "
        "Unsupported physical components are rejected during preview. Dense compiler limit: 2048 DOFs."),this);
    scopeDetails->setObjectName("piezoScopeDetails"); scopeDetails->hide(); layout->addWidget(scopeDetails);
    auto *buttons=new QHBoxLayout;
    // This row contributes to the outer scroll content's minimum width. Keep
    // captions compact so the result canvas can fit in a compact workflow dock.
    previewButton=new QPushButton(tr("Preview experiment"),this); previewButton->setObjectName("piezoPreview");
    previewButton->setToolTip(tr("Preview whole experiment"));previewButton->setAccessibleName(previewButton->toolTip());
    runButton=new QPushButton(tr("Run saved"),this); runButton->setObjectName("piezoRun");
    runButton->setToolTip(tr("Run saved experiment"));runButton->setAccessibleName(runButton->toolTip());
    buttons->addWidget(previewButton); buttons->addWidget(runButton); buttons->addStretch();
    auto *scopeButton=new QPushButton(tr("Scope / limits"),this);
    scopeButton->setToolTip(tr("Scope and limits"));scopeButton->setAccessibleName(scopeButton->toolTip());
    scopeButton->setObjectName("piezoScopeToggle"); scopeButton->setCheckable(true);
    connect(scopeButton,&QPushButton::toggled,scopeDetails,&QWidget::setVisible);
    buttons->addWidget(scopeButton);
    layout->insertLayout(1,buttons);
    status=note(tr("Draft changes require one complete preview and confirmation."),this);
    status->setObjectName("piezoStatus"); layout->addWidget(status);
    auto *previewPanel=new QWidget(this);previewPanel->setObjectName("piezoPreviewPanel");
    auto *previewLayout=new QVBoxLayout(previewPanel);previewLayout->setContentsMargins(0,0,0,0);
    changes=table({tr("Effect"),tr("Preview")},previewPanel,"piezoChanges");
    changes->setAccessibleName(tr("Experiment preview summary"));
    changes->setAccessibleDescription(tr("Review the complete experiment effects here before confirming once."));
    changes->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);
    changes->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
    changes->setWordWrap(true);changes->setMinimumHeight(104);changes->setMaximumHeight(120);
    changes->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    changes->setEditTriggers(QAbstractItemView::NoEditTriggers); changes->hide(); previewLayout->addWidget(changes);
    auto *previewDetailsRow=new QHBoxLayout;
    auto *expandPreview=new QPushButton(tr("Expand preview"),previewPanel);expandPreview->setObjectName("piezoExpandPreview");expandPreview->setCheckable(true);
    expandPreview->setAccessibleName(tr("Expand experiment preview details"));
    expandPreview->setAccessibleDescription(tr("Expand the summary to show more preview rows."));
    expandPreview->setToolTip(expandPreview->accessibleDescription());
    previewDetailsRow->addStretch();previewDetailsRow->addWidget(expandPreview);previewLayout->addLayout(previewDetailsRow);
    auto *confirmRow=new QHBoxLayout;confirmRow->addStretch();
    applyButton=new QPushButton(tr("Confirm preview"),previewPanel);applyButton->setObjectName("piezoConfirm");
    confirmRow->addWidget(applyButton);previewLayout->addLayout(confirmRow);previewPanel->hide();layout->addWidget(previewPanel);
    receipt=note({},this); receipt->setObjectName("piezoReceipt"); layout->addWidget(receipt);
    tabs=new QTabWidget(this); tabs->setObjectName("piezoPages"); layout->addWidget(tabs,1);
    connect(qApp,&QApplication::focusChanged,this,[this](QWidget *,QWidget *now){
        const auto focused=namedFocusWithin(now,tabs);
        if(focused.isEmpty())return;
        lastFocusedEditorName=focused;lastFocusedEditorPage=tabs->currentIndex();
    });
    connect(previewButton,&QPushButton::clicked,this,&PiezoWorkbenchWidget::previewExperiment);
    connect(runButton,&QPushButton::clicked,this,&PiezoWorkbenchWidget::previewRun);
    connect(applyButton,&QPushButton::clicked,this,&PiezoWorkbenchWidget::applyPreview);
    connect(expandPreview,&QPushButton::toggled,this,[this,expandPreview](bool expanded){
        if(expanded){
            const int expandedHeight=std::min(360,std::max(120,previewContentHeight(changes)));
            changes->setMinimumHeight(expandedHeight);changes->setMaximumHeight(expandedHeight);
        }else{
            changes->setMaximumHeight(120);changes->setMinimumHeight(104);
        }
        expandPreview->setText(expanded?tr("Collapse preview"):tr("Expand preview"));
        expandPreview->setAccessibleName(expanded?tr("Collapse experiment preview details"):tr("Expand experiment preview details"));
        const QString description=expanded?tr("Collapse the expanded preview summary."):tr("Expand the summary to show more preview rows.");
        expandPreview->setAccessibleDescription(description);expandPreview->setToolTip(description);
    });
    connect(changes,&QTableWidget::itemChanged,this,[this,expandPreview]{
        if(changes->property("piezoPreviewMeasureQueued").toBool())return;
        changes->setProperty("piezoPreviewMeasureQueued",true);
        QTimer::singleShot(0,this,[this,expandPreview]{
            changes->setProperty("piezoPreviewMeasureQueued",false);
            if(!changes->isVisible())return;
            changes->resizeRowsToContents();
            const bool needsExpansion=previewContentHeight(changes)>120;
            if(!needsExpansion&&expandPreview->isChecked())expandPreview->setChecked(false);
            expandPreview->setVisible(needsExpansion);
        });
    });
    auto *timer=new QTimer(this); timer->setInterval(500);
    connect(timer,&QTimer::timeout,this,[this]{
        // The periodic path uses cached QFileInfo stamps and native-result watches.
        // Full content hashes remain at explicit preview/apply/bind/export boundaries.
        const bool externalMetadataCurrent=externalSourcesMetadataCurrent();
        if((acceptedSeries||acceptedNumericSeries)&&(!externalMetadataCurrent||!acceptedSeriesCurrent(false,true)))clearAcceptedSeries(tr("Selected experiment or accepted record files changed. Select and certify the records again."));
        if(!approvedPreview.isEmpty()&&(!externalMetadataCurrent||!hasCurrentPreviewCurrent(false,true))) invalidatePreview(tr("Source or workspace changed. Preview again."));
        if(!externalMetadataCurrent) {
            if(!computedModes.isEmpty()){computedModes={};computedModesInput={};queueEditorRebuild();}
            invalidatePreview(tr("External force source or dependency changed. Reload its files explicitly, then preview again."));
        }
    }); timer->start();
    previewButton->setEnabled(false); applyButton->setEnabled(false); runButton->setEnabled(false);
}

void PiezoWorkbenchWidget::setSession(Session *value) {
    if(session==value) return;
    clearAcceptedSeries(tr("Source session changed. Select accepted records again."));
    if(session) disconnect(session,nullptr,this,nullptr);
    ++contextGeneration; refreshPending=false; session=value; loadedDocument={}; recipe={}; savedRecipe={}; selectedMembers={};computedModes={};computedModesInput={};appliedNativeResults.clear();
    invalidatePreview(tr("Select one native document."));
    if(session) {
        const auto changed=[this]{ clearAcceptedSeries(tr("Document or selection changed. Select accepted records again."));invalidatePreview(tr("Document or selection changed. Preview again.")); queueContextRefresh(); };
        connect(session,&Session::modelSelectionChanged,this,changed);
        connect(session,&Session::modelAdded,this,changed);
        connect(session,&Session::modelRemoved,this,changed);
        connect(session,&Session::documentRevisionChanged,this,changed);
        connect(session,&Session::runtimeDocumentRemoved,this,changed);
        connect(session,&Session::solverResultAvailable,this,[this](const QString &documentId,quint64 sourceRevision,
                                                                    const QString &artifactPath,bool applied){
            if(!applied||!session||session->getSelectedModelIDs().size()!=1)return;
            const auto current=session->modelCommandStamp(session->getSelectedModelIDs().first());
            const QFileInfo artifact(artifactPath);
            if(current.first!=documentId||current.second!=sourceRevision+1||!artifact.isFile()||
               artifact.isSymLink()||artifact.canonicalFilePath().isEmpty())return;
            if(appliedNativeResults.size()>=32)appliedNativeResults.clear();
            appliedNativeResults.insert(artifact.canonicalFilePath(),
                {{"documentId",documentId},{"sourceRevision",QString::number(sourceRevision)},
                 {"attachedRevision",QString::number(current.second)},
                 {"sessionEpoch",session->getModelCommandHost()->sessionEpoch()}});
        });
        // Native picks are captured once on source load; changing viewport picks
        // later does not silently replace an edited patch declaration.
        connect(session,&QObject::destroyed,this,changed);
    }
    refreshContext();
}

void PiezoWorkbenchWidget::queueContextRefresh() {
    if(refreshPending) return; refreshPending=true; auto generation=contextGeneration;
    QTimer::singleShot(0,this,[this,generation]{if(generation!=contextGeneration)return;refreshPending=false;refreshContext();});
}
void PiezoWorkbenchWidget::queueEditorRebuild() {
    if(editorRebuildPending)return; editorRebuildPending=true;
    const int activePage=tabs->currentIndex();int previousScrollX=0,previousScrollY=0;QString focusedEditorName;
    if(auto *activeScroll=qobject_cast<QScrollArea*>(tabs->widget(activePage))){
        previousScrollX=activeScroll->horizontalScrollBar()->value();previousScrollY=activeScroll->verticalScrollBar()->value();
    }
    focusedEditorName=namedFocusWithin(QApplication::focusWidget(),tabs);
    if(focusedEditorName.isEmpty()&&lastFocusedEditorPage==activePage)focusedEditorName=lastFocusedEditorName;
    QTimer::singleShot(0,this,[this,activePage,previousScrollX,previousScrollY,focusedEditorName]{
        editorRebuildPending=false;buildEditors(activePage,previousScrollX,previousScrollY,focusedEditorName);
    });
}
QJsonObject PiezoWorkbenchWidget::currentDocument(bool *temporarilyUnavailable) const {
    if(temporarilyUnavailable)*temporarilyUnavailable=false;
    if(!session||session->getSelectedModelIDs().size()!=1)return {};
    const auto stamp=session->modelCommandStamp(session->getSelectedModelIDs().first());
    auto *host=session->getModelCommandHost();
    const auto response=host->handleReadOnly(request("context.query",{{"target",QJsonObject{{"documentId",stamp.first},{"sessionEpoch",host->sessionEpoch()}}}}));
    const auto context=response.value("result").toObject();const auto documents=context.value("documents").toArray();
    if(response.value("status")!=QJsonValue("ok")||documents.size()!=1)return {};
    const auto d=documents.first().toObject();
    if(d.value("documentId")!=QJsonValue(stamp.first)||d.value("documentRevision").toDouble()!=double(stamp.second))return {};
    if(context.value("editingBlocked").toBool()||d.value("stateAvailability")!=QJsonValue("live")){
        // The host recognized this exact document but could not obtain a live
        // read. Keep an automatic result binding pending through this interval.
        if(temporarilyUnavailable)*temporarilyUnavailable=true;
        return {};
    }
    return d;
}
bool PiezoWorkbenchWidget::hasCurrentPreview() const {
    return hasCurrentPreviewCurrent(true);
}
bool PiezoWorkbenchWidget::hasCurrentPreviewCurrent(bool verifyExternalSources,bool externalSourcesKnownCurrent) const {
    const bool externalCurrent=externalSourcesKnownCurrent?true:(verifyExternalSources?externalSourcesCurrent():externalSourcesMetadataCurrent());
    if(!session||approvedPreview.isEmpty()||!externalCurrent||session->getModelCommandHost()->projectWorkspace()!=previewWorkspace)return false;
    const auto document=currentDocument();if(document.isEmpty())return false;
    for(const auto &key:{"documentId","sessionEpoch","documentRevision","project","draft"})
        if(document.value(key)!=previewDocument.value(key))return false;
    return true;
}
void PiezoWorkbenchWidget::invalidatePreview(const QString &reason) {
    approvedPreview={};previewDocument={};previewWorkspace.clear();previewCommand.clear();
    applyButton->setEnabled(false);receipt->clear();changes->setRowCount(0);changes->hide();
    if(auto *panel=changes->parentWidget())panel->hide();
    status->setText(reason);updateRunAvailability();
    applyButton->setText(tr("Confirm preview"));
}
void PiezoWorkbenchWidget::updateRunAvailability() {
    const auto grids=findChildren<QTableWidget*>();const bool validNumbers=std::none_of(grids.begin(),grids.end(),[](const auto *grid){return grid->property("invalidNumericEntry").toBool();});
    QString unavailable;
    if(!session)unavailable=tr("No source session.");else if(loadedDocument.isEmpty())unavailable=tr("Source document context is unavailable.");else if(savedRecipe.isEmpty())unavailable=tr("The actual source has no loadable saved experiment.");else if(recipe!=savedRecipe)unavailable=tr("The displayed draft differs from the authoritative saved experiment.");else if(!externalSourcesMetadataCurrent())unavailable=tr("External force source files changed or have not been certified.");else if(!forceDirectionsValid())unavailable=tr("A force direction is invalid for the source frame.");else if(!validNumbers)unavailable=tr("A numeric matrix entry is invalid.");
    const bool saved=unavailable.isEmpty();runButton->setProperty("unavailableReason",unavailable);
    runButton->setEnabled(saved);
    for(const auto &name:{"piezoExportMatrices","piezoExportAcceptedCsv","piezoShowAcceptedSeries"})if(auto *button=findChild<QPushButton*>(name))button->setEnabled(saved);
}
void PiezoWorkbenchWidget::refreshContext() {
    const auto document=currentDocument();
    if(!approvedPreview.isEmpty()&&!hasCurrentPreview())invalidatePreview(tr("Source changed. Preview again."));
    if(document.isEmpty()) {
        clearAcceptedSeries(tr("Select one saved source experiment before displaying accepted records."));
        previewButton->setEnabled(false);runButton->setEnabled(false);
        const int selectedCount=session?session->getSelectedModelIDs().size():0;
        // A selected model can still have an unavailable source document.
        // Keep the recovery path visible whenever there is no usable document.
        if(auto *chooseModelButton=findChild<QPushButton*>("piezoChooseModel"))chooseModelButton->show();
        if(selectedCount==0)source->setText(tr("0 models selected. Select one model in the Model tree."));
        else if(selectedCount>1)source->setText(tr("%1 models selected. Select exactly one model in the Model tree.").arg(selectedCount));
        else source->setText(tr("1 model selected, but its source document is not available."));
        return;
    }
    if(auto *chooseModelButton=findChild<QPushButton*>("piezoChooseModel"))chooseModelButton->hide();
    if(document.value("documentId")!=loadedDocument.value("documentId")||document.value("sessionEpoch")!=loadedDocument.value("sessionEpoch")
       ||document.value("documentRevision")!=loadedDocument.value("documentRevision"))
        loadDocument(document);
    else loadedDocument=document;
    source->setText(tr("Current model: %1 | revision %2 | %3")
        .arg(document.value("modelName").toString()).arg(document.value("documentRevision").toDouble(),0,'f',0)
        .arg(recipe.value("family").toString()=="axialRod"?tr("axial rod / cable"):recipe.value("family").toString()=="flatPlate"?tr("flat layered plate"):recipe.value("family").toString()=="solid3D"?tr("3D solid with piezo volumes"):tr("planar layered beam")));
    previewButton->setEnabled(!recipe.isEmpty());updateRunAvailability();
}
void PiezoWorkbenchWidget::loadDocument(const QJsonObject &document) {
    clearAcceptedSeries(tr("Source experiment changed. Select accepted records again."));
    loadedDocument=document;selectedMembers={};computedModes={};computedModesInput={};templateSynchronized=true;retiredAxialPorts={};axialDampingPresets={};
    const uint modelId=session->getSelectedModelIDs().first();RModel model;
    if(!sourceSnapshot(session,&model)){recipe={};savedRecipe={};loadedDocument={};return;}
    savedRecipe=RPiezoExperimentService::recipeFromModel(model);
    recipe=savedRecipe.isEmpty()?RPiezoExperimentService::exampleRecipe(model):savedRecipe;
    // Loading an authoritative saved recipe is an explicit certification boundary.
    // Seed the cheap UI metadata snapshot only after its external files pass full SHA.
    externalSourcesCurrent();
    synchronizedTemplate=recipe.value("template").toObject();
    if(savedRecipe.isEmpty()) {
        std::set<uint> ids;
        for(const auto &pick:session->getPickList().getItems(modelId))
            if(pick.getItemType()==PICK_ITEM_ELEMENT&&pick.getElementID()<model.getNElements())ids.insert(model.getElementPhysicalId(pick.getElementID()));
        for(uint id:ids)selectedMembers.append(double(id));
        if(!selectedMembers.isEmpty())recipe=RPiezoExperimentService::exampleRecipe(model,{},selectedMembers);
    }
    buildEditors();
    if(recipe.isEmpty())status->setText(tr("Use an empty document for a rod, beam, plate or 3D solid template, or a supported TRUSS1/BEAM1/QUAD1/TETRA1/HEXA1 mesh."));
}
void PiezoWorkbenchWidget::usePickedMembers() {
    if(!session||session->getSelectedModelIDs().size()!=1){status->setText(tr("Select one source document before using picked elements."));return;}
    const uint modelId=session->getSelectedModelIDs().first();RModel model;if(!sourceSnapshot(session,&model)){status->setText(tr("The source document is busy. Retry using the picked elements."));return;}
    const auto family=recipe.value("family").toString();
    const QString expected=family=="axialRod"?tr("TRUSS1 rods"):family=="flatPlate"?tr("QUAD1 host elements"):family=="solid3D"?tr("TETRA1/HEXA1 piezo volumes"):tr("BEAM1 host elements");
    std::set<uint> ids,unsupported;
    for(const auto &pick:session->getPickList().getItems(modelId))if(pick.getItemType()==PICK_ITEM_ELEMENT&&pick.getElementID()<model.getNElements()){
        const auto type=model.getElement(pick.getElementID()).getType();
        const bool supported=family=="axialRod"?type==R_ELEMENT_TRUSS1:family=="flatPlate"?type==R_ELEMENT_QUAD1:family=="solid3D"?(type==R_ELEMENT_TETRA1||type==R_ELEMENT_HEXA1):type==R_ELEMENT_BEAM1;
        const uint physicalId=model.getElementPhysicalId(pick.getElementID());(supported?ids:unsupported).insert(physicalId);
    }
    if(ids.empty()){
        status->setText(unsupported.empty()?tr("No elements are currently picked. Select %1 in the model, then use this action again.").arg(expected):
            tr("No supported %1 are picked. Select supported elements in the model, then retry.").arg(expected));return;
    }
    QJsonArray selection;for(uint id:ids)selection.append(double(id));
    // Only patch bindings change. Keep edited material, supports and control declarations.
    auto selected=RPiezoExperimentService::exampleRecipe(model,family,selection);
    if(selected.isEmpty()){status->setText(tr("The picked elements could not be used for this experiment. Select supported %1 and retry.").arg(expected));return;}
    selectedMembers=selection;
    auto updated=selected.value("members").toArray();const auto previous=recipe.value("members").toArray();
    for(int i=0;i<updated.size();++i){auto member=updated[i].toObject();
        for(const auto &old:previous){auto oldMember=old.toObject();if(oldMember.value("physicalElementId")!=member.value("physicalElementId"))continue;
            if(recipe.value("family")==QJsonValue("axialRod")){
                if(oldMember.value("kind")==member.value("kind"))member=oldMember;
                else {member["area"]=oldMember.value("area");for(const auto &material:recipe.value("materials").toArray()){
                    auto m=material.toObject();if(m.value("materialId")==oldMember.value("materialId")&&m.value("piezoelectric").toBool()==(member.value("kind")==QJsonValue("active")))member["materialId"]=oldMember.value("materialId");
                }}break;
            }
            auto oldLayers=oldMember.value("layers").toArray(),newLayers=member.value("layers").toArray();
            if(!oldLayers.isEmpty()&&!newLayers.isEmpty())newLayers[0]=oldLayers[0];
            if(newLayers.size()>1&&oldLayers.size()>1)newLayers=oldLayers;
            member["layers"]=newLayers;break;
        }updated[i]=member;
    }
    if(recipe.value("family")==QJsonValue("axialRod")){
        auto materials=recipe.value("materials").toArray();for(const auto &value:selected.value("materials").toArray()){
            bool found=false;for(const auto &old:materials)if(old.toObject()["materialId"]==value.toObject()["materialId"])found=true;if(!found)materials.append(value);
        }recipe["materials"]=materials;
        recipe["members"]=updated;rebindAxialPorts(previous,selected.value("ports").toArray());
        setValue({"members"},recipe.value("members"));
    }else setValue({"members"},updated);
    invalidatePreview(unsupported.empty()?tr("Picked elements captured. Review the device bindings and preview."):
        tr("Captured %1 supported element(s); ignored %2 unsupported picked element(s). Review the bindings and preview.").arg(ids.size()).arg(unsupported.size()));queueEditorRebuild();
}

QJsonValue PiezoWorkbenchWidget::valueAt(const QStringList &path) const {
    QJsonValue value(recipe);for(const auto &key:path)value=value.isArray()?value.toArray().at(key.toInt()):value.toObject()[key];return value;
}
void PiezoWorkbenchWidget::setValue(const QStringList &path,const QJsonValue &value) {
    if(rebuilding)return;
    if(valueAt(path)!=value)clearAcceptedSeries(tr("Experiment draft changed. Confirm it before selecting accepted records again."));
    const auto previousTemplate=recipe.value("template").toObject();
    const auto previousMembers=recipe.value("members").toArray();
    if(path==QStringList{"damping","kind"}&&recipe.value("family")==QJsonValue("axialRod")){const auto damping=recipe.value("damping").toObject();axialDampingPresets[damping.value("kind").toString()]=damping;}
    recipe=replaceAt(recipe,path,0,value).toObject();invalidatePreview(tr("Draft changed. Preview the whole experiment again."));
    if(path==QStringList{"template","kind"}&&value==QJsonValue("axialTruss")&&recipe.value("template").toObject()["elements"].toInt()<2)recipe=replaceAt(recipe,{"template","elements"},0,2).toObject();
    if(path.first()=="members"&&recipe.value("family")==QJsonValue("axialRod"))rebindAxialPorts(previousMembers);
    if(path.first()=="template")synchronizeTemplate(previousTemplate);
    if(path.first()=="frame")queueEditorRebuild();
    if(!computedModes.isEmpty()&&modalInputRecipe(recipe)!=computedModesInput){computedModes={};computedModesInput={};queueEditorRebuild();}
}
void PiezoWorkbenchWidget::rebindAxialPorts(const QJsonArray &previousMembers,const QJsonArray &defaults) {
    std::set<QString> oldIds,newIds;
    for(const auto &m:previousMembers)if(m.toObject()["kind"]==QJsonValue("active"))oldIds.insert(m.toObject()["portId"].toString());
    for(const auto &m:recipe.value("members").toArray())if(m.toObject()["kind"]==QJsonValue("active"))newIds.insert(m.toObject()["portId"].toString());
    std::set<QString> defaultIds;for(const auto &p:defaults)defaultIds.insert(p.toObject()["portId"].toString());
    auto ports=recipe.value("ports").toArray();for(int i=int(ports.size())-1;i>=0;--i){auto p=ports[i].toObject();const auto id=p.value("portId").toString();if((oldIds.count(id)||defaultIds.count(id))&&!newIds.count(id)){retiredAxialPorts[id]=p;ports.removeAt(i);}}
    for(const auto &id:newIds){if(id.isEmpty())continue;bool found=false;for(const auto &v:ports)if(v.toObject()["portId"].toString()==id)found=true;if(found)continue;
        auto p=retiredAxialPorts[id].toObject();if(p.isEmpty())for(const auto &v:defaults)if(v.toObject()["portId"].toString()==id){p=v.toObject();break;}
        if(p.isEmpty())p=QJsonObject{{"portId",id},{"role","actuatorSensor"},{"readout","actuatorCharge"},{"initialValue",0.}};
        ports.append(p);
    }recipe["ports"]=ports;
}
void PiezoWorkbenchWidget::synchronizeTemplate(const QJsonObject &previousTemplate) {
    templateSynchronized=false;
    RModel source;if(!sourceSnapshot(session,&source)){status->setText(tr("Source is busy. Retry updating the template before preview."));return;}
    try{
        const auto defaultsBefore=templateRecipe(source,synchronizedTemplate.isEmpty()?previousTemplate:synchronizedTemplate);
        auto templateParameters=recipe.value("template").toObject();if(templateParameters.value("kind").toString().startsWith("solid")){if(!templateParameters.contains("patchThickness"))templateParameters["patchThickness"]=templateParameters.value("thickness").toDouble()*.05;}else templateParameters.remove("patchThickness");recipe["template"]=templateParameters;
        const auto defaultsAfter=templateRecipe(source,templateParameters);
        auto updated=recipe;
        const auto oldTemplate=synchronizedTemplate.isEmpty()?previousTemplate:synchronizedTemplate,newTemplate=recipe.value("template").toObject();
        const auto oldMesh=RPiezoExperimentService::templateCandidate(source,oldTemplate),newMesh=RPiezoExperimentService::templateCandidate(source,newTemplate);QJsonObject nodeMapping;
        for(uint i=0;i<oldMesh.getNNodes();++i){const auto &oldNode=oldMesh.getNode(i);double best=1e300;uint nearest=0;
            for(uint j=0;j<newMesh.getNNodes();++j){const auto &newNode=newMesh.getNode(j);const double x=oldNode.getX()/oldTemplate.value("length").toDouble()-newNode.getX()/newTemplate.value("length").toDouble(),y=oldNode.getY()/oldTemplate.value("width").toDouble()-newNode.getY()/newTemplate.value("width").toDouble();const double z=oldNode.getZ()/oldTemplate.value("thickness").toDouble()-newNode.getZ()/newTemplate.value("thickness").toDouble();const double distance=x*x+y*y+z*z;if(distance<best){best=distance;nearest=j;}}
            nodeMapping[QString::number(oldMesh.getNodePhysicalId(i))]=double(newMesh.getNodePhysicalId(nearest));
        }
        for(const auto &key:{"supports","backgroundForces","initialDisplacements","initialVelocities","workpoint","excitation"})updated[key]=remapNodeBindings(updated[key],nodeMapping);
        for(const auto &key:{"family","frame"})updated[key]=defaultsAfter[key];
        auto materials=recipe.value("materials").toArray();for(const auto &v:defaultsAfter.value("materials").toArray()){
            bool found=false;for(int i=0;i<materials.size();++i)if(materials[i].toObject()["materialId"]==v.toObject()["materialId"]){found=true;
                for(const auto &oldDefault:defaultsBefore.value("materials").toArray())if(materials[i]==oldDefault){materials[i]=v;break;}
            }if(!found)materials.append(v);
        }updated["materials"]=materials;
        const auto beforePorts=defaultsBefore.value("ports").toArray(),afterPorts=defaultsAfter.value("ports").toArray(),oldPorts=recipe.value("ports").toArray();QJsonArray ports;QJsonObject portMapping;
        for(int i=0;i<afterPorts.size();++i){auto p=afterPorts[i].toObject();if(i<beforePorts.size()){
                auto before=beforePorts[i].toObject();portMapping[before.value("portId").toString()]=p.value("portId");
                for(const auto &v:oldPorts){auto custom=v.toObject();if(custom.value("portId")!=before.value("portId"))continue;
                    for(auto it=custom.begin();it!=custom.end();++it)if(it.key()!="portId"&&it.value()!=before[it.key()])p[it.key()]=it.value();break;
                }
            }ports.append(p);
        }
        for(const auto &v:oldPorts){bool defaultPort=false;for(const auto &p:beforePorts)if(p.toObject()["portId"]==v.toObject()["portId"])defaultPort=true;if(!defaultPort)ports.append(v);}
        updated["ports"]=ports;
        if(recipe.value("supports")==defaultsBefore.value("supports"))updated["supports"]=defaultsAfter.value("supports");
        auto design=remapNodeBindings(recipe.value("design"),nodeMapping).toObject();
        if(design.value("outputs")==defaultsBefore.value("design").toObject()["outputs"])design["outputs"]=defaultsAfter.value("design").toObject()["outputs"];
        updated["design"]=design;
        // Material/port/control/force declarations and custom layers stay edited.
        // Default layers follow dimensions; new topology receives new defaults.
        auto members=defaultsAfter.value("members").toArray(),old=recipe.value("members").toArray(),before=defaultsBefore.value("members").toArray();
        const bool axial=defaultsAfter.value("family")==QJsonValue("axialRod");
        for(int i=0;i<members.size()&&i<old.size()&&i<before.size();++i){auto member=members[i].toObject();auto afterLayers=member.value("layers").toArray();auto oldLayers=old[i].toObject()["layers"].toArray(),beforeLayers=before[i].toObject()["layers"].toArray();
            if(axial){
                if(defaultsBefore.value("family")==QJsonValue("axialRod")){
                    auto custom=old[i].toObject();for(const auto &key:{"kind","materialId","area","portId","polarity","initialTension"})
                        if(custom.contains(key)&&custom[key]!=before[i].toObject()[key])member[key]=custom[key];
                    if(portMapping.contains(member.value("portId").toString()))member["portId"]=portMapping[member.value("portId").toString()];
                    if(member.value("kind")!=QJsonValue("active")){member.remove("portId");member.remove("polarity");}
                    if(member.value("kind")!=QJsonValue("cable"))member.remove("initialTension");
                }
                members[i]=member;continue;
            }
            if(defaultsBefore.value("family")==QJsonValue("axialRod")||defaultsBefore.value("family")!=defaultsAfter.value("family")){members[i]=member;continue;}
            QJsonArray layers;for(int j=0;j<oldLayers.size();++j){
                if(j<beforeLayers.size()&&j<afterLayers.size()&&oldLayers[j]==beforeLayers[j])layers.append(afterLayers[j]);
                else {auto custom=oldLayers[j].toObject();if(defaultsAfter.value("family")==QJsonValue("flatPlate"))custom["width"]=1.;layers.append(custom);}
            }member["layers"]=layers;members[i]=member;
        }
        updated["members"]=members;recipe=updated;
        if(axial)rebindAxialPorts(before,afterPorts);
        // Electrical references follow regenerated electrode identities.
        auto controller=recipe.value("controller").toObject();auto outputs=controller.value("selectedOutputs").toArray();for(int i=0;i<outputs.size();++i)if(portMapping.contains(outputs[i].toString()))outputs[i]=portMapping[outputs[i].toString()];if(controller.contains("selectedOutputs")){controller["selectedOutputs"]=outputs;recipe["controller"]=controller;}
        auto workpoint=recipe.value("workpoint").toObject();auto references=workpoint.value("ports").toArray();for(int i=0;i<references.size();++i){auto p=references[i].toObject();if(portMapping.contains(p.value("portId").toString())){p["portId"]=portMapping[p.value("portId").toString()];references[i]=p;}}if(workpoint.contains("ports")){workpoint["ports"]=references;recipe["workpoint"]=workpoint;}
        templateSynchronized=true;synchronizedTemplate=recipe.value("template").toObject();
        computedModes={};computedModesInput={};queueEditorRebuild();
        status->setText(tr("Template mesh and default devices updated together. Nodal bindings follow the nearest relative mesh position; custom declarations were preserved. Review the updated nodes before whole preview."));
    }catch(const std::exception &e){status->setText(tr("Template update failed; preview is blocked until corrected: %1").arg(QString::fromUtf8(e.what())));}
}
QDoubleSpinBox *PiezoWorkbenchWidget::numberEditor(const QStringList &path,QWidget *parent,const QString &name,double minimum,double maximum) {
    auto *spin=new ScientificDoubleSpinBox(parent);spin->setObjectName(piezoEditorObjectName(path,name));spin->setAccessibleName(piezoEditorAccessibleName(path));spin->setRange(minimum,maximum);
    spin->setValue(valueAt(path).toDouble());spin->setKeyboardTracking(false);
    connect(spin->findChild<QLineEdit*>(),&QLineEdit::textEdited,this,[this]{clearAcceptedSeries(tr("Numeric draft editing started. Confirm the experiment before selecting accepted records again."));});
    connect(spin,&QDoubleSpinBox::valueChanged,this,[this,path](double v){setValue(path,v);});return spin;
}
QComboBox *PiezoWorkbenchWidget::choiceEditor(const QStringList &path,const QStringList &values,QWidget *parent,const QString &name,bool rebuild) {
    auto *combo=new QComboBox(parent);combo->setObjectName(piezoEditorObjectName(path,name));combo->setAccessibleName(piezoEditorAccessibleName(path));for(const auto &v:values)combo->addItem(v,v);
    combo->setCurrentIndex(combo->findData(valueAt(path).toString()));
    connect(combo,&QComboBox::currentIndexChanged,this,[this,path,combo,rebuild]{setValue(path,combo->currentData().toString());if(rebuild)queueEditorRebuild();});return combo;
}
QComboBox *PiezoWorkbenchWidget::nodeEditor(const QStringList &path,QWidget *parent) {
    auto *combo=new QComboBox(parent);combo->setObjectName(piezoEditorObjectName(path));combo->setAccessibleName(piezoEditorAccessibleName(path));for(const auto &v:nodeCatalog){const auto n=v.toObject();combo->addItem(n.value("label").toString(),n.value("id").toDouble());}
    combo->setCurrentIndex(combo->findData(valueAt(path).toDouble()));
    connect(combo,&QComboBox::currentIndexChanged,this,[this,path,combo]{setValue(path,combo->currentData().toDouble());});return combo;
}
QTableWidget *PiezoWorkbenchWidget::matrixEditor(const QStringList &path,int rows,int columns,bool flat,QWidget *parent,const QString &name) {
    auto *grid=new QTableWidget(rows,columns,parent);grid->setObjectName(piezoEditorObjectName(path,name));grid->setAccessibleName(piezoEditorAccessibleName(path));grid->setMinimumHeight(140);grid->setMaximumHeight(300);
    QStringList rowLabels,columnLabels;for(int i=0;i<rows;++i)rowLabels.append(QString::number(i+1));for(int i=0;i<columns;++i)columnLabels.append(QString::number(i+1));
    grid->setVerticalHeaderLabels(rowLabels);grid->setHorizontalHeaderLabels(columnLabels);
    grid->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    const auto values=valueAt(path).toArray();
    for(int r=0;r<rows;++r)for(int c=0;c<columns;++c){
        const double v=flat?values.at(r*columns+c).toDouble():values.at(r).toArray().at(c).toDouble();
        grid->setItem(r,c,new QTableWidgetItem(QString::number(v,'g',17)));
    }
    connect(grid,&QTableWidget::cellChanged,this,[this,path,grid,flat,columns](int r,int c){
        clearAcceptedSeries(tr("Numeric matrix draft changed. Confirm the experiment before selecting accepted records again."));
        const QSignalBlocker blocker(grid);
        bool ok=false;const double value=grid->item(r,c)->text().toDouble(&ok);
        if(!ok||!std::isfinite(value)){
            grid->item(r,c)->setBackground(QColor(255,210,210));
            grid->setProperty("invalidNumericEntry",true);invalidatePreview(tr("Matrix entries must be finite numbers. Correct the highlighted entry."));return;
        }
        grid->item(r,c)->setBackground(QBrush());
        bool valid=true;for(int i=0;i<grid->rowCount();++i)for(int j=0;j<grid->columnCount();++j){
            bool cellOk=false;double x=grid->item(i,j)->text().toDouble(&cellOk);valid=valid&&cellOk&&std::isfinite(x);
        }
        grid->setProperty("invalidNumericEntry",!valid);
        auto cellPath=path;if(flat)cellPath.append(rowIndex(r*columns+c));else cellPath<<rowIndex(r)<<rowIndex(c);
        setValue(cellPath,value);
    });return grid;
}

int PiezoWorkbenchWidget::compiledFreeDofs() const {
    if(!session||session->getSelectedModelIDs().size()!=1||recipe.isEmpty())return 0;
    auto provisional=recipe;provisional["controller"]=QJsonObject{{"kind","none"}};
    provisional["damping"]=QJsonObject{{"kind","none"}};
    // Dimension inspection is pure. The actual host preview still requires the
    // explicit replacement checkbox before accepting any legacy replacement.
    provisional["replaceLegacyConditions"]=true;
    RModel model;if(!sourceSnapshot(session,&model))return 0;
    const auto result=RPiezoExperimentService::preview(model,provisional);
    return result.valid?result.report.value("freeDofs").toInt():0;
}

void PiezoWorkbenchWidget::buildEditors() {
    int previousScrollX=0,previousScrollY=0;QString focusedEditorName;
    const int activePage=tabs->currentIndex();
    if(auto *activeScroll=qobject_cast<QScrollArea*>(tabs->widget(activePage))){
        previousScrollX=activeScroll->horizontalScrollBar()->value();previousScrollY=activeScroll->verticalScrollBar()->value();
    }
    focusedEditorName=namedFocusWithin(QApplication::focusWidget(),tabs);
    if(focusedEditorName.isEmpty()&&lastFocusedEditorPage==activePage)focusedEditorName=lastFocusedEditorName;
    buildEditors(activePage,previousScrollX,previousScrollY,focusedEditorName);
}
void PiezoWorkbenchWidget::buildEditors(int activePage,int previousScrollX,int previousScrollY,const QString &focusedEditorName) {
    rebuilding=true;
    resultPort=nullptr;resultQuantity=nullptr;resultReference=nullptr;resultView=nullptr;resultWindow=nullptr;resultRemoveMean=nullptr;resultCoverage=nullptr;resultPlot=nullptr;
    while(tabs->count()){auto *page=tabs->widget(0);tabs->removeTab(0);delete page;}
    if(recipe.isEmpty()){rebuilding=false;return;}
    nodeCatalog={};
    if(session&&session->getSelectedModelIDs().size()==1){
        try {RModel model;if(!sourceSnapshot(session,&model))throw std::runtime_error("Source is busy");
            if(recipe.contains("template"))model=RPiezoExperimentService::templateCandidate(model,recipe.value("template").toObject());
            for(uint i=0;i<model.getNNodes();++i)nodeCatalog.append(QJsonObject{{"id",double(model.getNodePhysicalId(i))},{"label",nodeLabel(i,model.getNode(i))}});
        }catch(...){status->setText(tr("Template dimensions or source are invalid."));}
    }
    const auto page=[this](const QString &title){
        auto *scroll=new QScrollArea(tabs);scroll->setWidgetResizable(true);auto *content=new QWidget(scroll);
        auto *v=new QVBoxLayout(content);scroll->setWidget(content);tabs->addTab(scroll,title);return v;
    };
    auto *devices=page(tr("Source / devices / materials"));
    if(recipe.value("family")!=QJsonValue("axialRod")){
        auto *axes=new QGroupBox(recipe.value("family")==QJsonValue("flatPlate")?tr("Local axes of plate"):recipe.value("family")==QJsonValue("solid3D")?tr("Material and electrode axes of solid"):tr("Local axes of beam"),this);auto *v=new QVBoxLayout(axes);
        v->addWidget(note(tr("Global X / Y / Z components of each local axis (dimensionless). Preview verifies an orthonormal right-handed frame against the actual source mesh geometry."),axes));
        for(const auto &axis:{"ex","ey","ez"}){v->addWidget(note(tr("Local %1").arg(QString(axis).toUpper()),axes));v->addWidget(matrixEditor({"frame",axis},1,3,true,axes,QString("piezoFrame%1").arg(axis)));}devices->addWidget(axes);
    }
    devices->addWidget(note(tr("Example material tensors are synthetic SI demonstration data, not vendor or calibrated hardware data. "
        "Material form, full tensors, provenance, density, patch thickness/offset/coverage and shared electrode bindings are editable."),this));
    auto *replace=new QCheckBox(tr("Replace all existing legacy loads, supports, initial and environment conditions"),this);
    replace->setObjectName("piezoReplaceLegacy");replace->setChecked(recipe.value("replaceLegacyConditions").toBool());devices->addWidget(replace);
    connect(replace,&QCheckBox::toggled,this,[this](bool v){setValue({"replaceLegacyConditions"},v);});
    if(recipe.contains("template")){
        auto *group=new QGroupBox(tr("Generate mesh in this empty document"),this);auto *form=new QFormLayout(group);
        form->addRow(tr("Template"),choiceEditor({"template","kind"},{"axialTruss","cantileverBeam","flatPlate","solidHex","solidTet"},group,"piezoTemplateKind"));
        for(const auto &key:{"length","width","thickness"})form->addRow(tr("%1 (m)").arg(key),numberEditor({"template",key},group,QString("piezoTemplate%1").arg(key),1e-8,1e6));
        if(recipe.value("family")==QJsonValue("solid3D"))form->addRow(tr("Piezo patch thickness (m)"),numberEditor({"template","patchThickness"},group,"piezoTemplatePatchThickness",1e-8,1e6));
        auto *count=new QSpinBox(group);count->setObjectName("piezoTemplateElements");count->setRange(recipe.value("family")==QJsonValue("axialRod")?2:1,recipe.value("family")==QJsonValue("solid3D")?32:100);count->setValue(recipe.value("template").toObject()["elements"].toInt());
        connect(count,&QSpinBox::valueChanged,this,[this](int n){setValue({"template","elements"},n);});form->addRow(tr("Elements"),count);
        auto *generate=new QPushButton(tr("Retry template synchronization"),group);generate->setObjectName("piezoUpdateTemplate");form->addRow(generate);
        connect(generate,&QPushButton::clicked,this,[this]{invalidatePreview(tr("Synchronizing template..."));synchronizeTemplate(recipe.value("template").toObject());});devices->addWidget(group);
    }
    const bool axial=recipe.value("family")==QJsonValue("axialRod");
    const bool solid=recipe.value("family")==QJsonValue("solid3D");
    auto *pickButton=new QPushButton(axial?tr("Use currently picked rods as piezo devices"):solid?tr("Use picked solid volumes as piezo material"):tr("Use currently picked host elements for patches"),this);pickButton->setObjectName("piezoUsePickedMembers");
    pickButton->setEnabled(!recipe.contains("template"));connect(pickButton,&QPushButton::clicked,this,&PiezoWorkbenchWidget::usePickedMembers);devices->addWidget(pickButton);
    QStringList materialIds,portIds{""};for(const auto &v:recipe.value("materials").toArray())materialIds.append(v.toObject()["materialId"].toString());
    for(const auto &v:recipe.value("ports").toArray())portIds.append(v.toObject()["portId"].toString());
    if(axial){
        devices->addWidget(note(tr("Each axial device replaces its selected rod. Active rods use a piezoelectric material and electrode; passive rods and cables use a mechanical material. Area and cable initial tension are physical SI values."),this));
        auto *rods=table({tr("Rod element"),tr("Kind"),tr("Material"),tr("Area (m²)"),tr("Electrode"),tr("Polarity"),tr("Initial tension (N)")},this,"piezoAxialMembers");
        const auto members=recipe.value("members").toArray();rods->setRowCount(members.size());
        for(int i=0;i<members.size();++i){auto path=QStringList{"members",rowIndex(i)};auto member=members[i].toObject();auto *label=new QTableWidgetItem(tr("Rod %1 (physical ID %2)").arg(i+1).arg(static_cast<qulonglong>(member.value("physicalElementId").toDouble())));label->setFlags(Qt::ItemIsEnabled);label->setToolTip(label->text());rods->setItem(i,0,label);
            auto *kind=choiceEditor(path+QStringList{"kind"},{"active","passive","cable"},rods,QString("piezoAxialKind%1").arg(i),true);rods->setCellWidget(i,1,kind);
            connect(kind,&QComboBox::currentIndexChanged,this,[this,path,kind]{auto member=valueAt(path).toObject();const bool active=kind->currentData().toString()=="active";
                bool compatible=false;for(const auto &v:recipe.value("materials").toArray()){auto m=v.toObject();if(m.value("materialId")==member.value("materialId")&&m.value("piezoelectric").toBool()==active)compatible=true;}
                if(!compatible)for(const auto &v:recipe.value("materials").toArray()){auto m=v.toObject();if(m.value("piezoelectric").toBool()==active){member["materialId"]=m.value("materialId");break;}}
                if(active){if(member.value("portId").toString().isEmpty())member["portId"]=QString("rod_%1").arg(member.value("physicalElementId").toDouble(),0,'f',0);if(!member.contains("polarity"))member["polarity"]=1;}
                else{member.remove("portId");member.remove("polarity");}
                if(kind->currentData().toString()=="cable"){if(!member.contains("initialTension"))member["initialTension"]=0.;}else member.remove("initialTension");
                setValue(path,member);
            });
            rods->setCellWidget(i,2,choiceEditor(path+QStringList{"materialId"},materialIds,rods,QString("piezoAxialMaterial%1").arg(i)));
            rods->setCellWidget(i,3,numberEditor(path+QStringList{"area"},rods,QString("piezoAxialArea%1").arg(i),1e-12,1e6));
            if(member.value("kind")==QJsonValue("active")){
                rods->setCellWidget(i,4,choiceEditor(path+QStringList{"portId"},portIds,rods,QString("piezoAxialPort%1").arg(i)));
                auto *polarity=new QComboBox(rods);polarity->addItem("+",1);polarity->addItem("−",-1);polarity->setCurrentIndex(member.value("polarity").toInt()==-1?1:0);
                connect(polarity,&QComboBox::currentIndexChanged,this,[this,path,polarity]{setValue(path+QStringList{"polarity"},polarity->currentData().toInt());});rods->setCellWidget(i,5,polarity);
            }
            if(member.value("kind")==QJsonValue("cable"))rods->setCellWidget(i,6,numberEditor(path+QStringList{"initialTension"},rods,{},0));
        }devices->addWidget(rods);
    }else if(solid){
        devices->addWidget(note(tr("Each row is one meshed solid volume, not an added shell layer. Piezo volumes must share conforming mechanical nodes with the host. Electrode planes are material-frame z coordinates. TET4 and affine HEX8 are supported; warped HEX8 is rejected."),this));
        auto *volumes=table({tr("Solid volume"),tr("Material"),tr("Electrode"),tr("Lower electrode z (m)"),tr("Upper electrode z (m)"),tr("Polarity")},this,"piezoSolidVolumes");
        const auto members=recipe.value("members").toArray();volumes->setRowCount(members.size());
        for(int i=0;i<members.size();++i){const auto member=members[i].toObject();auto path=QStringList{"members",rowIndex(i),"layers","0"};
            auto *label=new QTableWidgetItem(tr("Volume %1").arg(member.value("physicalElementId").toDouble(),0,'f',0));label->setFlags(Qt::ItemIsEnabled);volumes->setItem(i,0,label);
            volumes->setCellWidget(i,1,choiceEditor(path+QStringList{"materialId"},materialIds,volumes));
            volumes->setCellWidget(i,2,choiceEditor(path+QStringList{"portId"},portIds,volumes));
            volumes->setCellWidget(i,3,numberEditor(path+QStringList{"lowerZ"},volumes));volumes->setCellWidget(i,4,numberEditor(path+QStringList{"upperZ"},volumes));
            auto *polarity=new QComboBox(volumes);polarity->addItem("+",1);polarity->addItem("−",-1);polarity->setCurrentIndex(valueAt(path+QStringList{"polarity"}).toInt()==-1?1:0);
            connect(polarity,&QComboBox::currentIndexChanged,this,[this,path,polarity]{setValue(path+QStringList{"polarity"},polarity->currentData().toInt());});volumes->setCellWidget(i,5,polarity);
        }devices->addWidget(volumes);
    }else{
    const bool plate = recipe.value("family") == QJsonValue("flatPlate");
    if (plate) devices->addWidget(note(tr("Plate patch area follows the host mesh and the xi / eta coverage below."), this));
    auto *layersTable=table({tr("Host element"),tr("Material"),tr("Thickness (m)"),tr("Offset (m)"),tr("Width (m)"),tr("Electrode"),tr("Polarity"),tr("xi min"),tr("xi max"),tr("eta min"),tr("eta max")},this,"piezoLayers");
    layersTable->setColumnHidden(4, plate);
    const auto members=recipe.value("members").toArray();int row=0;
    for(int i=0;i<members.size();++i){const auto member=members[i].toObject();const auto layers=member["layers"].toArray();
        for(int j=0;j<layers.size();++j){layersTable->insertRow(row);auto path=QStringList{"members",rowIndex(i),"layers",rowIndex(j)};const auto layer=layers[j].toObject();
            auto *label=new QTableWidgetItem(tr("%1 %2 (physical ID %3) / layer %4").arg(recipe.value("family")==QJsonValue("flatPlate")?tr("Plate cell"):tr("Beam element")).arg(i+1).arg(static_cast<qulonglong>(member.value("physicalElementId").toDouble())).arg(j+1));label->setFlags(label->flags()&~Qt::ItemIsEditable);label->setData(Qt::UserRole,i);label->setData(Qt::UserRole+1,j);label->setToolTip(label->text());layersTable->setItem(row,0,label);
            layersTable->setCellWidget(row,1,choiceEditor(path+QStringList{"materialId"},materialIds,layersTable));
            const double lo=layer.value("lowerZ").toDouble(),hi=layer.value("upperZ").toDouble();
            auto *thickness=new ScientificDoubleSpinBox(layersTable),*offset=new ScientificDoubleSpinBox(layersTable);
            thickness->setRange(1e-12,1e6);offset->setRange(-1e6,1e6);
            thickness->setValue(hi-lo);offset->setValue((hi+lo)/2);thickness->setKeyboardTracking(false);offset->setKeyboardTracking(false);
            thickness->setObjectName(QString("piezoLayerThickness%1_%2").arg(i).arg(j));
            const auto update=[this,path,thickness,offset]{setValue(path+QStringList{"lowerZ"},offset->value()-thickness->value()/2);setValue(path+QStringList{"upperZ"},offset->value()+thickness->value()/2);};
            connect(thickness,&QDoubleSpinBox::valueChanged,this,update);connect(offset,&QDoubleSpinBox::valueChanged,this,update);
            layersTable->setCellWidget(row,2,thickness);layersTable->setCellWidget(row,3,offset);
            if (!plate) layersTable->setCellWidget(row,4,numberEditor(path+QStringList{"width"},layersTable,{},1e-12,1e6));
            layersTable->setCellWidget(row,5,choiceEditor(path+QStringList{"portId"},portIds,layersTable));
            auto *polarity=new QComboBox(layersTable);polarity->addItem("+",1);polarity->addItem("−",-1);polarity->setCurrentIndex(layer.value("polarity").toInt()==-1?1:0);
            connect(polarity,&QComboBox::currentIndexChanged,this,[this,path,polarity]{setValue(path+QStringList{"polarity"},polarity->currentData().toInt());});layersTable->setCellWidget(row,6,polarity);
            for(int c=0;c<4;++c)layersTable->setCellWidget(row,7+c,numberEditor(path+QStringList{"coverage",rowIndex(c)},layersTable,{},-1,1));++row;
        }
    }devices->addWidget(layersTable);
    auto *layerButtons=new QHBoxLayout;auto *addLayer=new QPushButton(tr("Add patch to selected host"),this),*removeLayer=new QPushButton(tr("Remove selected layer"),this);layerButtons->addWidget(addLayer);layerButtons->addWidget(removeLayer);devices->addLayout(layerButtons);
    connect(addLayer,&QPushButton::clicked,this,[this,layersTable]{int row=layersTable->currentRow();if(row<0)return;int i=layersTable->item(row,0)->data(Qt::UserRole).toInt();auto members=recipe.value("members").toArray();auto member=members[i].toObject();auto layers=member.value("layers").toArray();double upper=0;for(const auto &l:layers)upper=std::max(upper,l.toObject()["upperZ"].toDouble());QString mat;for(const auto &v:recipe.value("materials").toArray())if(v.toObject()["piezoelectric"].toBool()){mat=v.toObject()["materialId"].toString();break;}if(mat.isEmpty())return;auto ports=recipe.value("ports").toArray();layers.append(QJsonObject{{"materialId",mat},{"lowerZ",upper},{"upperZ",upper+.0005},{"width",layers.isEmpty()?.1:layers.first().toObject()["width"].toDouble()},{"portId",ports.isEmpty()?QString():ports.first().toObject()["portId"].toString()},{"polarity",1},{"coverage",QJsonArray{-1.,1.,-1.,1.}}});setValue({"members",rowIndex(i),"layers"},layers);queueEditorRebuild();});
    connect(removeLayer,&QPushButton::clicked,this,[this,layersTable]{int row=layersTable->currentRow();if(row<0)return;auto *item=layersTable->item(row,0);int i=item->data(Qt::UserRole).toInt(),j=item->data(Qt::UserRole+1).toInt();auto path=QStringList{"members",rowIndex(i),"layers"};auto layers=valueAt(path).toArray();layers.removeAt(j);setValue(path,layers);queueEditorRebuild();});
    }
    const auto materials=recipe.value("materials").toArray();
    for(int i=0;i<materials.size();++i){auto path=QStringList{"materials",rowIndex(i)};auto material=materials[i].toObject();
        auto *group=new QGroupBox(tr("Material: %1").arg(material.value("materialId").toString()),this);auto *v=new QVBoxLayout(group);auto *form=new QFormLayout;
        form->addRow(tr("Form"),choiceEditor(path+QStringList{"form"},{"stressCharge","strainCharge"},group));
        form->addRow(tr("Density (kg/m³)"),numberEditor(path+QStringList{"density"},group,{},1e-12));
        auto *piezo=new QCheckBox(tr("Piezoelectric material"),group);piezo->setChecked(material.value("piezoelectric").toBool());connect(piezo,&QCheckBox::toggled,this,[this,path](bool v){setValue(path+QStringList{"piezoelectric"},v);});form->addRow(piezo);
        form->addRow(tr("Source"),choiceEditor(path+QStringList{"sourceKind"},{"example","vendor","calibration"},group));
        for(const auto &key:{"sourceId","sourceSha256","notes"}){auto *edit=new QLineEdit(material[key].toString(),group);edit->setObjectName(QString("piezoMaterial%1%2").arg(i).arg(key));
            connect(edit,&QLineEdit::textChanged,this,[this,path,key](const QString &s){setValue(path+QStringList{key},s);});form->addRow(QString(key),edit);}
        v->addLayout(form);
        const QStringList names{tr("cᴱ (Pa) / sᴱ (1/Pa), Voigt 6 × 6"),tr("e (C/m²) / d (m/V), 3 × 6"),tr("εˢ / εᵀ (F/m), 3 × 3")};
        const QStringList keys{"elasticityOrCompliance","coupling","permittivity"};
        for(int k=0;k<3;++k){v->addWidget(note(names[k],group));v->addWidget(matrixEditor(path+QStringList{keys[k]},k==0?6:3,k==2?3:6,true,group,QString("piezoMaterial%1%2").arg(i).arg(keys[k])));}
        devices->addWidget(group);
    }devices->addStretch();

    auto *sensors=page(tr("Sensors / electrodes / supports"));
    auto *ports=table({tr("Electrode"),tr("Role"),tr("Ideal readout"),tr("Cf (F)"),tr("Initial value (V or C)"),tr("Qref (C)"),tr("Bias (V)"),tr("Amplifier polarity")},this,"piezoPorts");
    const auto portArray=recipe.value("ports").toArray();ports->setRowCount(portArray.size());
    for(int i=0;i<portArray.size();++i){auto path=QStringList{"ports",rowIndex(i)};auto *name=new QTableWidgetItem(portArray[i].toObject()["portId"].toString());name->setFlags(name->flags()&~Qt::ItemIsEditable);ports->setItem(i,0,name);
        ports->setCellWidget(i,1,choiceEditor(path+QStringList{"role"},{"actuator","sensor","actuatorSensor"},ports));
        ports->setCellWidget(i,2,choiceEditor(path+QStringList{"readout"},{"none","openCircuitVoltage","shortCircuitCharge","idealChargeAmplifier","actuatorCharge"},ports));
        const QStringList fields{"Cf","initialValue","Qref","VoutBias"};for(int j=0;j<fields.size();++j)ports->setCellWidget(i,j+3,numberEditor(path+QStringList{fields[j]},ports));
        auto *polarity=new QComboBox(ports);polarity->addItem("+",1);polarity->addItem("−",-1);polarity->setCurrentIndex(portArray[i].toObject()["amplifierPolarity"].toInt(1)==-1?1:0);
        connect(polarity,&QComboBox::currentIndexChanged,this,[this,path,polarity]{setValue(path+QStringList{"amplifierPolarity"},polarity->currentData().toInt());});ports->setCellWidget(i,7,polarity);
    }sensors->addWidget(ports);
    auto *portButtons=new QHBoxLayout;auto *addPort=new QPushButton(tr("Add electrode"),this),*removePort=new QPushButton(tr("Remove selected electrode"),this);portButtons->addWidget(addPort);portButtons->addWidget(removePort);sensors->addLayout(portButtons);
    connect(addPort,&QPushButton::clicked,this,[this]{auto a=recipe.value("ports").toArray();std::set<QString> names;for(const auto &p:a)names.insert(p.toObject()["portId"].toString());int n=1;while(names.count(QString("electrode_%1").arg(n)))++n;a.append(QJsonObject{{"portId",QString("electrode_%1").arg(n)},{"role","sensor"},{"readout","openCircuitVoltage"},{"initialValue",0.}});setValue({"ports"},a);queueEditorRebuild();});
    removePort->setObjectName("piezoRemovePort");
    connect(removePort,&QPushButton::clicked,this,[this,ports]{int i=ports->currentRow();auto a=recipe.value("ports").toArray();if(i<0||i>=a.size())return;const QString id=a.at(i).toObject().value("portId").toString();
        for(const auto &m:recipe.value("members").toArray()){
            if(m.toObject().value("portId").toString()==id){status->setText(tr("This electrode is still bound to an axial rod. Rebind the rod before removing the electrode."));return;}
            for(const auto &l:m.toObject()["layers"].toArray())if(l.toObject().value("portId").toString()==id){status->setText(tr("This electrode is still bound to a device layer. Rebind or remove that layer before removing the electrode."));return;}
        }a.removeAt(i);setValue({"ports"},a);queueEditorRebuild();});
    sensors->addWidget(note(tr("Voltage source actions are action records. Independent open-circuit voltage / short-circuit charge / ideal charge-amplifier signals use the assembled piezoelectric coupling. Shared circuit parasitics are unsupported."),this));
    auto *supports=table({tr("Bound node"),tr("Physical DOF"),tr("Value (m / rad)")},this,"piezoSupports");auto supportArray=recipe.value("supports").toArray();supports->setRowCount(supportArray.size());
    for(int i=0;i<supportArray.size();++i){auto path=QStringList{"supports",rowIndex(i)};supports->setCellWidget(i,0,nodeEditor(path+QStringList{"physicalNodeId"},supports));
        supports->setCellWidget(i,1,choiceEditor(path+QStringList{"dof"},axial?QStringList{"UX","UY","UZ"}:QStringList{"UX","UY","UZ","RX","RY","RZ"},supports));supports->setCellWidget(i,2,numberEditor(path+QStringList{"value"},supports));}
    sensors->addWidget(supports);auto *supportButtons=new QHBoxLayout;auto *addSupport=new QPushButton(tr("Add support"),this),*removeSupport=new QPushButton(tr("Remove selected support"),this);
    supportButtons->addWidget(addSupport);supportButtons->addWidget(removeSupport);sensors->addLayout(supportButtons);
    connect(addSupport,&QPushButton::clicked,this,[this]{if(nodeCatalog.isEmpty())return;auto a=recipe.value("supports").toArray();a.append(QJsonObject{{"physicalNodeId",nodeCatalog.first().toObject()["id"]},{"dof","UX"},{"value",0.}});setValue({"supports"},a);queueEditorRebuild();});
    connect(removeSupport,&QPushButton::clicked,this,[this,supports]{int i=supports->currentRow();auto a=recipe.value("supports").toArray();if(i>=0&&i<a.size()){a.removeAt(i);setValue({"supports"},a);queueEditorRebuild();}});
    addOutputEditor(sensors);sensors->addStretch();

    auto *modes=page(tr("Modes / damping / export"));
    const int free=compiledFreeDofs();modes->addWidget(note(tr("Actual source compiler: %1 free mechanical DOFs. Mode indices are zero-based; selection changes controller state dimensions. Numerical residuals, clusters and matrix export are certified by the production design backend during whole preview.").arg(free),this));
    auto *compute=new QPushButton(tr("Compute frequencies and mode clusters from this FE draft"),this);compute->setObjectName("piezoComputeModes");connect(compute,&QPushButton::clicked,this,&PiezoWorkbenchWidget::computeModes);modes->addWidget(compute);
    if(!computedModes.isEmpty()){
        double residual=0;for(const auto &v:computedModes.value("modalResiduals").toArray())residual=std::max(residual,v.toDouble());
        modes->addWidget(note(tr("Production backend: %1 modes, %2 clusters; maximum eigen residual %3; mass orthogonality error %4.")
            .arg(computedModes.value("omega").toArray().size()).arg(computedModes.value("clusters").toArray().size()).arg(residual,0,'g',6).arg(computedModes.value("massOrthogonalityError").toDouble(),0,'g',6),this));
        const auto frequencies=computedModes.value("omega").toArray();double slowest=std::numeric_limits<double>::infinity(),fastest=0.;
        for(const auto &value:frequencies){const double omega=value.toDouble();if(std::isfinite(omega)&&omega>0){slowest=std::min(slowest,omega);fastest=std::max(fastest,omega);}}
        if(fastest>0&&std::isfinite(slowest)){
            const double dt=(2*std::acos(-1.)/20)/fastest,steps=std::ceil(60*(fastest/slowest));
            const bool affordable=std::isfinite(dt)&&dt>=1e-12&&dt<=1e6&&std::isfinite(steps)&&steps>=1&&steps<=8192;
            auto *suggestion=note(tr("Actual full FE modes suggest dt=%1 s (20 samples per fastest mode), %2 FE steps for at least 3 slowest-mode periods. This is an accuracy starting point, not a convergence certificate. The automatic suggestion has an 8192-step budget; longer windows are not silently shortened. Current custom dt and window stay unchanged until you choose Apply. Zero excitation and zero initial state produce zero response.")
                .arg(dt,0,'g',10).arg(steps,0,'g',10),this);suggestion->setObjectName("piezoGridSuggestion");modes->addWidget(suggestion);
            auto *use=new QPushButton(affordable?tr("Apply suggested integration grid"):tr("Suggested full FE window exceeds the automatic budget or supported clock range"),this);use->setObjectName("piezoApplySuggestedGrid");use->setEnabled(affordable);
            use->setProperty("suggestedDtS",dt);use->setProperty("suggestedSteps",steps);modes->addWidget(use);
            connect(use,&QPushButton::clicked,this,[this,dt,steps,affordable]{if(!affordable||computedModes.isEmpty()||modalInputRecipe(recipe)!=computedModesInput)return;
                setValue({"dt"},dt);setValue({"steps"},int(steps));const auto controller=recipe.value("controller").toObject();
                if(controller.value("kind")==QJsonValue("discrete_state_space"))setValue({"controller","samplingPeriod"},dt*controller.value("controlEveryFESteps").toInt(1));
                queueEditorRebuild();status->setText(tr("Suggested grid copied into the draft. Excitation and initial conditions are unchanged; preview and confirm the whole experiment."));
            });
        }
    }
    auto *allModes=new QCheckBox(tr("Use all automatic modes"),this);allModes->setObjectName("piezoAllModes");allModes->setChecked(!recipe.value("design").toObject().contains("modeIndices"));modes->addWidget(allModes);
    auto *modeTable=table({tr("Use"),tr("Mode"),tr("Frequency (Hz)"),tr("Cluster"),tr("Damping ratio")},this,"piezoModes");modeTable->setRowCount(free);
    auto selected=recipe.value("design").toObject()["modeIndices"].toArray();auto ratios=recipe.value("damping").toObject()["ratios"].toArray();
    const auto clusters=computedModes.value("clusters").toArray(),omega=computedModes.value("omega").toArray();
    const auto clusterFor=[clusters](int mode){for(int i=0;i<clusters.size();++i)if(clusters[i].toArray().contains(mode))return i;return -1;};
    for(int i=0;i<free;++i){auto *item=new QTableWidgetItem;item->setFlags(Qt::ItemIsEnabled|Qt::ItemIsUserCheckable);item->setCheckState(allModes->isChecked()||selected.contains(i)?Qt::Checked:Qt::Unchecked);modeTable->setItem(i,0,item);
        for(int col=1;col<=3;++col){QString text=col==1?tr("Mode %1").arg(i+1):col==2?(i<omega.size()?QString::number(omega[i].toDouble()/(2*std::acos(-1.)),'g',10):tr("Compute modes")):(clusterFor(i)>=0?QString::number(clusterFor(i)+1):QString());auto *value=new QTableWidgetItem(text);value->setFlags(Qt::ItemIsEnabled);modeTable->setItem(i,col,value);}
        auto *ratio=new ScientificDoubleSpinBox(modeTable);ratio->setRange(0,1);ratio->setValue(i<ratios.size()?ratios[i].toDouble():.01);ratio->setEnabled(recipe.value("damping").toObject()["kind"]==QJsonValue("perMode"));modeTable->setCellWidget(i,4,ratio);
        connect(ratio,&QDoubleSpinBox::valueChanged,this,[this,modeTable]{QJsonArray a;for(int j=0;j<modeTable->rowCount();++j)a.append(qobject_cast<QDoubleSpinBox*>(modeTable->cellWidget(j,4))->value());setValue({"damping","ratios"},a);});
    }modes->addWidget(modeTable);
    const auto selectModes=[this,modeTable,allModes]{auto design=recipe.value("design").toObject();if(allModes->isChecked())design.remove("modeIndices");else{QJsonArray a;for(int i=0;i<modeTable->rowCount();++i)if(modeTable->item(i,0)->checkState()==Qt::Checked)a.append(i);design["modeIndices"]=a;}setValue({"design"},design);};
    const auto automaticModes=[modeTable,allModes,selectModes]{
        const QSignalBlocker blocker(modeTable);for(int i=0;i<modeTable->rowCount();++i){auto *item=modeTable->item(i,0);
            item->setFlags(allModes->isChecked()?Qt::ItemFlags(Qt::ItemIsEnabled):Qt::ItemFlags(Qt::ItemIsEnabled|Qt::ItemIsUserCheckable));
            if(allModes->isChecked())item->setCheckState(Qt::Checked);
        }selectModes();
    };
    // Initialization changes only presentation; it must not modify saved declarations.
    for(int i=0;i<modeTable->rowCount();++i)modeTable->item(i,0)->setFlags(allModes->isChecked()?Qt::ItemFlags(Qt::ItemIsEnabled):Qt::ItemFlags(Qt::ItemIsEnabled|Qt::ItemIsUserCheckable));
    connect(allModes,&QCheckBox::toggled,this,automaticModes);connect(modeTable,&QTableWidget::cellChanged,this,[modeTable,selectModes,clusters,clusterFor](int row,int column){
        if(column!=0)return;const auto blocker=QSignalBlocker(modeTable);const int cluster=clusterFor(row);
        if(cluster>=0)for(const auto &index:clusters[cluster].toArray())modeTable->item(index.toInt(),0)->setCheckState(modeTable->item(row,0)->checkState());
        selectModes();
    });
    modes->addWidget(note(tr("Computed near-repeated modes are selected as complete certified clusters. Recompute after material, electrode, support or geometry changes."),this));
    auto *dampingForm=new QFormLayout;auto *dampingChoice=choiceEditor({"damping","kind"},{"none","uniformModal","perMode","perCluster","rayleigh","imported"},this,"piezoDamping",true);dampingForm->addRow(tr("Physical damping authority"),dampingChoice);
    connect(dampingChoice,&QComboBox::currentIndexChanged,this,[this,dampingChoice,free]{
        const auto kind=dampingChoice->currentData().toString();auto damping=recipe.value("damping").toObject();
        if(recipe.value("family")==QJsonValue("axialRod")){damping=axialDampingPresets[kind].toObject();damping["kind"]=kind;
            if(kind=="rayleigh"){if(!damping.contains("rayleighAlpha"))damping["rayleighAlpha"]=0.;if(!damping.contains("rayleighBeta"))damping["rayleighBeta"]=0.;}
        }
        if(kind=="uniformModal"&&!damping.contains("uniformRatio"))damping["uniformRatio"]=.01;
        if(kind=="perMode"&&damping.value("ratios").toArray().size()!=free)damping["ratios"]=zeros(free,.01);
        if(kind=="perCluster"&&!computedModes.isEmpty()){
            QJsonArray ends;for(const auto &cluster:computedModes.value("clusters").toArray())ends.append(cluster.toArray().last().toInt()+1);
            if(damping.value("clusterEnds")!=QJsonValue(ends))damping["ratios"]=zeros(ends.size(),.01);
            damping["clusterEnds"]=ends;
        }
        setValue({"damping"},damping);
    });
    const auto damping=recipe.value("damping").toObject();const auto dampingKind=damping.value("kind").toString();
    if(dampingKind=="uniformModal")dampingForm->addRow(tr("Uniform damping ratio"),numberEditor({"damping","uniformRatio"},this,"piezoDampingRatio",0,1));
    if(dampingKind=="rayleigh"){dampingForm->addRow(tr("α (1/s)"),numberEditor({"damping","rayleighAlpha"},this,{},0));dampingForm->addRow(tr("β (s)"),numberEditor({"damping","rayleighBeta"},this,{},0));}
    modes->addLayout(dampingForm);
    if(dampingKind=="perCluster"){
        if(computedModes.isEmpty())modes->addWidget(note(tr("Compute actual modes before editing per-cluster damping. Preview is blocked until the complete cluster boundaries are certified."),this));
        else{auto *clusterTable=table({tr("Certified cluster"),tr("Frequency range (Hz)"),tr("Damping ratio")},this,"piezoDampingClusters");clusterTable->setRowCount(clusters.size());
            auto ratios=damping.value("ratios").toArray();for(int i=0;i<clusters.size();++i){auto indices=clusters[i].toArray();clusterTable->setItem(i,0,new QTableWidgetItem(tr("Cluster %1 (%2 modes)").arg(i+1).arg(indices.size())));
                clusterTable->setItem(i,1,new QTableWidgetItem(QString("%1 – %2").arg(omega[indices.first().toInt()].toDouble()/(2*std::acos(-1.)),0,'g',8).arg(omega[indices.last().toInt()].toDouble()/(2*std::acos(-1.)),0,'g',8)));
                auto *ratio=numberEditor({"damping","ratios",rowIndex(i)},clusterTable,QString("piezoClusterRatio%1").arg(i),0,1);clusterTable->setCellWidget(i,2,ratio);
            }clusterTable->setEditTriggers(QAbstractItemView::NoEditTriggers);modes->addWidget(clusterTable);
        }
    }
    if(dampingKind=="imported"){
        auto *dimension=new QSpinBox(this);dimension->setRange(1,2048);dimension->setValue(std::max(1,damping.value("importedDimension").toInt()));auto *resize=new QPushButton(tr("Resize imported full C grid"),this);modes->addWidget(dimension);modes->addWidget(resize);
        connect(resize,&QPushButton::clicked,this,[this,dimension]{int n=dimension->value();if(n>128){status->setText(tr("The interactive full C grid is limited to 128 coordinates."));return;}setValue({"damping","importedDimension"},n);setValue({"damping","importedMatrix"},zeros(n*n));queueEditorRebuild();});
        int n=damping.value("importedDimension").toInt();if(n>0&&n<=128&&damping.value("importedMatrix").toArray().size()==n*n)modes->addWidget(matrixEditor({"damping","importedMatrix"},n,n,true,this,"piezoImportedC"));
        for(const auto &key:{"sourceId","sourceSha256"}){auto *edit=new QLineEdit(damping[key].toString(),this);modes->addWidget(edit);edit->setPlaceholderText(QString(key));connect(edit,&QLineEdit::textChanged,this,[this,key](const QString &s){setValue({"damping",key},s);});}
    }
    auto *matrixExport=new QPushButton(tr("Export current saved FE / ROM matrix package…"),this);matrixExport->setObjectName("piezoExportMatrices");matrixExport->setEnabled(!savedRecipe.isEmpty()&&recipe==savedRecipe);connect(matrixExport,&QPushButton::clicked,this,&PiezoWorkbenchWidget::exportMatrixPackage);modes->addWidget(matrixExport);
    modes->addWidget(note(tr("Export uses the actual current bound experiment and production numerical package. Changing the draft invalidates the old mass/stiffness/readout, modes, control design, results and restart state together."),this));modes->addStretch();

    auto *controlPage=page(tr("Controllers / state source"));auto *controlForm=new QFormLayout;
    auto controller=recipe.value("controller").toObject();auto *kind=new QComboBox(this);kind->setObjectName("piezoController");kind->setAccessibleName(tr("Controller"));for(const auto &s:QStringList{"none","velocity_feedback","PI","PD","PID","discrete_state_space","LQR","MPC","external"})kind->addItem(s,s);kind->setCurrentIndex(kind->findData(controller.value("kind").toString()));controlForm->addRow(tr("Controller"),kind);
    auto *state=new QComboBox(this);state->setObjectName("piezoStateSource");state->setAccessibleName(tr("State source"));state->addItem(tr("Simulation full FE state projection"),"full_fe");state->addItem(tr("Only selected sensor signals"),"sensor_only");state->setCurrentIndex(controller.value("stateSource")==QJsonValue("sensor_only")?1:0);controlForm->addRow(tr("State source"),state);
    connect(kind,&QComboBox::currentIndexChanged,this,[this,kind,state]{configureController(kind->currentData().toString(),state->currentData().toString());});
    connect(state,&QComboBox::currentIndexChanged,this,[this,kind,state]{configureController(kind->currentData().toString(),state->currentData().toString());});
    if(controller.value("kind")!=QJsonValue("none")){
        auto *interval=new QSpinBox(this);interval->setObjectName("piezoControlEveryFESteps");interval->setRange(1,1000000);interval->setValue(std::max(1,controller.value("controlEveryFESteps").toInt(1)));
        connect(interval,&QSpinBox::valueChanged,this,[this](int n){setValue({"controller","controlEveryFESteps"},n);if(recipe.value("controller").toObject()["kind"]==QJsonValue("discrete_state_space"))setValue({"controller","samplingPeriod"},recipe.value("dt").toDouble()*n);});controlForm->addRow(tr("Control every N FE steps"),interval);
        if(controller.value("kind")==QJsonValue("discrete_state_space")){
            auto *dimension=new QSpinBox(this);dimension->setObjectName("piezoDiscreteStates");dimension->setRange(1,128);dimension->setValue(std::max(1,int(controller.value("Ac").toArray().size())));controlForm->addRow(tr("Controller states"),dimension);
            connect(dimension,&QSpinBox::valueChanged,this,[this,state](int n){setValue({"controller","Ac"},diagonal(n,n));configureController("discrete_state_space",state->currentData().toString());});
        }
    }controlPage->addLayout(controlForm);
    const auto outputs=feedbackCatalog(recipe);
    if(controller.value("kind")!=QJsonValue("none")){
        auto *selection=table({tr("Use feedback"),tr("Signal"),tr("Units")},this,"piezoControllerOutputs");selection->setRowCount(outputs.size());
        for(int i=0;i<outputs.size();++i){auto o=outputs[i].toObject();auto *item=new QTableWidgetItem;item->setFlags(Qt::ItemIsEnabled|Qt::ItemIsUserCheckable);item->setCheckState(!controller.contains("selectedOutputs")||controller.value("selectedOutputs").toArray().contains(o.value("id"))?Qt::Checked:Qt::Unchecked);selection->setItem(i,0,item);selection->setItem(i,1,new QTableWidgetItem(o.value("id").toString()));selection->setItem(i,2,new QTableWidgetItem(o.value("unit").toString()));}
        selection->setEditTriggers(QAbstractItemView::NoEditTriggers);controlPage->addWidget(selection);
        connect(selection,&QTableWidget::cellChanged,this,[this,selection,outputs](int,int){QJsonArray selected;for(int i=0;i<outputs.size();++i)if(selection->item(i,0)->checkState()==Qt::Checked)selected.append(outputs[i].toObject()["id"]);setValue({"controller","selectedOutputs"},selected);});
        auto *resize=new QPushButton(tr("Size controller matrices from current FE / modes / feedback selection"),this);resize->setObjectName("piezoSizeController");controlPage->addWidget(resize);
        connect(resize,&QPushButton::clicked,this,[this,kind,state]{configureController(kind->currentData().toString(),state->currentData().toString());});
    }
    for(auto it=controller.begin();it!=controller.end();++it){
        const auto key=it.key();if(QStringList{"kind","stateSource","controlEveryFESteps","selectedOutputs","inputUnits","outputUnits","observer","external"}.contains(key))continue;
        auto path=QStringList{"controller",key};controlPage->addWidget(note(key,this));
        if(it.value().isDouble())controlPage->addWidget(numberEditor(path,this,"piezoController"+key));
        else if(it.value().isArray()){const auto a=it.value().toArray();if(!a.isEmpty()){int cols=a.first().isArray()?a.first().toArray().size():a.size();int rows=a.first().isArray()?a.size():1;
            if(rows<=128&&cols<=128)controlPage->addWidget(matrixEditor(path,rows,cols,!a.first().isArray(),this,"piezoController"+key));}}
    }
    const auto observer=controller.value("observer").toObject();for(auto it=observer.begin();it!=observer.end();++it){auto a=it.value().toArray();controlPage->addWidget(note(tr("Observer %1").arg(it.key()),this));
        if(!a.isEmpty()&&a.size()<=128)controlPage->addWidget(matrixEditor({"controller","observer",it.key()},a.first().isArray()?a.size():1,a.first().isArray()?a.first().toArray().size():a.size(),!a.first().isArray(),this,"piezoObserver"+it.key()));}
    if(controller.value("kind")==QJsonValue("external")){state->setCurrentIndex(1);state->setEnabled(false);addExternalProgramEditor(controlPage,{"controller","external"},"controller");}
    controlPage->addWidget(note(tr("LQR requires the production Riccati certificate; MPC requires the production constrained QP certificate. Sensor-only LQR/MPC requires an observable/detectable selected measurement set and an explicit observer. Preview failure never substitutes another controller."),this));
    if(controller.value("kind")==QJsonValue("MPC"))controlPage->addWidget(note(tr("MPC prediction holds the target voltage over each control period. Rate limiting at individual FE steps can make the actual response differ from that prediction. The QP certificate applies to this prediction model."),this));controlPage->addStretch();

    auto *excitation=page(tr("Excitation / initial state"));auto *grid=new QFormLayout;
    auto *dt=numberEditor({"dt"},this,"piezoTimeStep",1e-12,1e6);grid->addRow(tr("FE time step (s)"),dt);
    connect(dt,&QDoubleSpinBox::valueChanged,this,[this](double t){auto c=recipe.value("controller").toObject();if(c.value("kind")==QJsonValue("discrete_state_space"))setValue({"controller","samplingPeriod"},t*c.value("controlEveryFESteps").toInt(1));});
    auto *steps=new QSpinBox(this);steps->setObjectName("piezoSteps");steps->setRange(1,1000000);steps->setValue(recipe.value("steps").toInt());connect(steps,&QSpinBox::valueChanged,this,[this](int n){setValue({"steps"},n);});grid->addRow(tr("FE steps"),steps);excitation->addLayout(grid);
    addExcitationEditor(excitation);addExternalExcitationEditors(excitation);addNodeFields(excitation,"backgroundForces",tr("Background forces (N / N m)"));addNodeFields(excitation,"initialDisplacements",tr("Initial displacement (m / rad)"));addNodeFields(excitation,"initialVelocities",tr("Initial velocity (m/s / rad/s)"));addWorkpointEditor(excitation);
    excitation->addWidget(note(tr("Zero excitation is the default. Endpoint force evaluation and physical damping remain separate from held controller commands. Results are supplied by the actual native solver job and the existing result viewer; restart is invalidated by experiment edits."),this));excitation->addStretch();
    auto *results=page(tr("Accepted results"));
    auto *csvExport=new QPushButton(tr("Export accepted records to CSV…"),this);csvExport->setObjectName("piezoExportAcceptedCsv");csvExport->setToolTip(tr("Export selected accepted native records to CSV…"));csvExport->setAccessibleName(csvExport->toolTip());csvExport->setEnabled(!savedRecipe.isEmpty()&&recipe==savedRecipe);connect(csvExport,&QPushButton::clicked,this,&PiezoWorkbenchWidget::exportAcceptedTimeSeries);results->addWidget(csvExport);
    addAcceptedResultEditor(results);
    // Long field labels and named-port items must not impose their combined
    // natural widths on the scroll content and hence on the real canvas.
    auto *resultContent=results->parentWidget();
    for(auto *form:resultContent->findChildren<QFormLayout*>()){
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    }
    for(auto *choice:resultContent->findChildren<QComboBox*>()){
        choice->setMinimumContentsLength(12);
        choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        choice->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        // The popup retains every full item; expose the selected full name
        // when the closed selector has less space than its natural width.
        choice->setToolTip(choice->currentText());
        connect(choice,&QComboBox::currentTextChanged,choice,&QWidget::setToolTip);
    }
    if(activePage>=0&&activePage<tabs->count())tabs->setCurrentIndex(activePage);
    rebuilding=false;updateRunAvailability();
    if(activePage>=0&&activePage<tabs->count())QTimer::singleShot(0,this,[this,activePage,previousScrollX,previousScrollY,focusedEditorName]{
        if(activePage>=tabs->count())return;auto *page=tabs->widget(activePage);auto *scroll=qobject_cast<QScrollArea*>(page);
        QWidget *restoredFocus=focusedEditorName.isEmpty()?nullptr:page->findChild<QWidget*>(focusedEditorName);
        if(restoredFocus)restoredFocus->setFocus(Qt::OtherFocusReason);
        if(scroll){scroll->horizontalScrollBar()->setValue(previousScrollX);scroll->verticalScrollBar()->setValue(previousScrollY);}
    });
}

void PiezoWorkbenchWidget::addNodeFields(QVBoxLayout *layout,const QString &field,const QString &label) {
    layout->addWidget(note(label,this));auto *t=table({tr("Bound node"),"X","Y","Z","RX","RY","RZ"},this,"piezo"+field);auto a=recipe.value(field).toArray();t->setRowCount(a.size());
    for(int i=0;i<a.size();++i){auto path=QStringList{field,rowIndex(i)};t->setCellWidget(i,0,nodeEditor(path+QStringList{"physicalNodeId"},t));for(int j=0;j<6;++j){auto *spin=numberEditor(path+QStringList{"values",rowIndex(j)},t);spin->setEnabled(recipe.value("family")!=QJsonValue("axialRod")||j<3);t->setCellWidget(i,j+1,spin);}}layout->addWidget(t);
    auto *buttons=new QHBoxLayout;auto *add=new QPushButton(tr("Add node field"),this),*remove=new QPushButton(tr("Remove selected field"),this);buttons->addWidget(add);buttons->addWidget(remove);layout->addLayout(buttons);
    connect(add,&QPushButton::clicked,this,[this,field]{if(nodeCatalog.isEmpty())return;auto a=recipe.value(field).toArray();a.append(QJsonObject{{"physicalNodeId",nodeCatalog.last().toObject()["id"]},{"values",zeros(6)}});setValue({field},a);queueEditorRebuild();});
    connect(remove,&QPushButton::clicked,this,[this,field,t]{auto a=recipe.value(field).toArray();int i=t->currentRow();if(i>=0&&i<a.size()){a.removeAt(i);setValue({field},a);queueEditorRebuild();}});
}
void PiezoWorkbenchWidget::addWorkpointEditor(QVBoxLayout *layout) {
    auto *explicitReference=new QCheckBox(tr("Use an explicit equilibrium working point (advanced)"),this);
    explicitReference->setObjectName("piezoExplicitWorkpoint");auto workpoint=recipe.value("workpoint").toObject();explicitReference->setChecked(workpoint.value("available").toBool());layout->addWidget(explicitReference);
    connect(explicitReference,&QCheckBox::toggled,this,[this](bool available){
        QJsonArray nodes,ports;
        if(available){for(const auto &n:nodeCatalog)nodes.append(QJsonObject{{"physicalNodeId",n.toObject()["id"]},{"q0",zeros(6)},{"f0",zeros(6)}});
            for(const auto &p:recipe.value("ports").toArray())ports.append(QJsonObject{{"portId",p.toObject()["portId"]},{"V0",0.},{"Q0",0.}});}
        setValue({"workpoint"},QJsonObject{{"available",available},{"nodes",nodes},{"ports",ports}});queueEditorRebuild();
    });
    layout->addWidget(note(tr("The source compiler verifies the full force/electrical equilibrium residual. This reference is used for absolute and increment readouts, modal projection and initial FE state; declaring a working point does not bypass equilibrium validation."),this));
    if(!workpoint.value("available").toBool())return;
    auto nodes=workpoint.value("nodes").toArray();
    for(const auto &field:QStringList{"q0","f0"}){
        layout->addWidget(note(field=="q0"?tr("Reference displacement q₀ (m / rad)"):tr("Reference force f₀ (N / N m)"),this));
        auto *t=table({tr("Bound node"),"X","Y","Z","RX","RY","RZ"},this,"piezoWorkpoint"+field);t->setRowCount(nodes.size());
        for(int i=0;i<nodes.size();++i){auto path=QStringList{"workpoint","nodes",rowIndex(i)};t->setCellWidget(i,0,nodeEditor(path+QStringList{"physicalNodeId"},t));
            for(int c=0;c<6;++c){auto *spin=numberEditor(path+QStringList{field,rowIndex(c)},t);spin->setEnabled(recipe.value("family")!=QJsonValue("axialRod")||c<3);t->setCellWidget(i,c+1,spin);}}
        layout->addWidget(t);
    }
    auto ports=workpoint.value("ports").toArray();auto *t=table({tr("Electrode"),tr("V₀ (V)"),tr("Q₀ (C)")},this,"piezoWorkpointPorts");t->setRowCount(ports.size());
    for(int i=0;i<ports.size();++i){auto path=QStringList{"workpoint","ports",rowIndex(i)};auto *item=new QTableWidgetItem(ports[i].toObject()["portId"].toString());item->setFlags(Qt::ItemIsEnabled);t->setItem(i,0,item);
        t->setCellWidget(i,1,numberEditor(path+QStringList{"V0"},t));t->setCellWidget(i,2,numberEditor(path+QStringList{"Q0"},t));}
    layout->addWidget(t);
}
void PiezoWorkbenchWidget::addOutputEditor(QVBoxLayout *layout) {
    layout->addWidget(note(tr("Additional nodal measurements. Electrode readouts are included automatically by the native design compiler."),this));
    auto *t=table({tr("Signal name"),tr("Bound node"),tr("Quantity"),tr("DOF"),tr("Unit")},this,"piezoOutputs");auto outputs=recipe.value("design").toObject()["outputs"].toArray();t->setRowCount(outputs.size());
    for(int i=0;i<outputs.size();++i){auto path=QStringList{"design","outputs",rowIndex(i)};auto o=outputs[i].toObject();auto *name=new QLineEdit(o.value("id").toString(),t);connect(name,&QLineEdit::textChanged,this,[this,path](const QString &s){setValue(path+QStringList{"id"},s);});t->setCellWidget(i,0,name);t->setCellWidget(i,1,nodeEditor(path+QStringList{"physicalNodeId"},t));
        auto *quantity=choiceEditor(path+QStringList{"quantity"},{"displacement","velocity","acceleration"},t);auto *dof=choiceEditor(path+QStringList{"dof"},recipe.value("family")==QJsonValue("axialRod")?QStringList{"X","Y","Z"}:QStringList{"X","Y","Z","RX","RY","RZ"},t);t->setCellWidget(i,2,quantity);t->setCellWidget(i,3,dof);auto *unit=new QLabel(o.value("unit").toString(),t);t->setCellWidget(i,4,unit);
        const auto units=[this,path,quantity,dof,unit]{QString u=dof->currentData().toString().startsWith('R')?"rad":"m";if(quantity->currentData().toString()=="velocity")u+="/s";if(quantity->currentData().toString()=="acceleration")u+="/s^2";setValue(path+QStringList{"unit"},u);unit->setText(u);};
        connect(quantity,&QComboBox::currentIndexChanged,this,units);connect(dof,&QComboBox::currentIndexChanged,this,units);
    }layout->addWidget(t);auto *buttons=new QHBoxLayout;auto *add=new QPushButton(tr("Add nodal measurement"),this),*remove=new QPushButton(tr("Remove selected measurement"),this);buttons->addWidget(add);buttons->addWidget(remove);layout->addLayout(buttons);
    connect(add,&QPushButton::clicked,this,[this]{if(nodeCatalog.isEmpty())return;auto a=recipe.value("design").toObject()["outputs"].toArray();a.append(QJsonObject{{"id",QString("measurement_%1").arg(a.size()+1)},{"physicalNodeId",nodeCatalog.last().toObject()["id"]},{"quantity","velocity"},{"dof","Z"},{"unit","m/s"}});setValue({"design","outputs"},a);queueEditorRebuild();});
    connect(remove,&QPushButton::clicked,this,[this,t]{auto a=recipe.value("design").toObject()["outputs"].toArray();int i=t->currentRow();if(i>=0&&i<a.size()){a.removeAt(i);setValue({"design","outputs"},a);queueEditorRebuild();}});
}

void PiezoWorkbenchWidget::configureController(const QString &kind,const QString &stateSource) {
    if(rebuilding)return;
    const auto previous=recipe.value("controller").toObject();int m=0;
    for(const auto &v:recipe.value("ports").toArray())if(v.toObject()["role"]!=QJsonValue("sensor"))++m;
    auto outputs=feedbackCatalog(recipe);QJsonArray selected;
    if(previous.value("kind")==QJsonValue(kind)&&previous.contains("selectedOutputs"))selected=previous.value("selectedOutputs").toArray();
    else for(const auto &v:outputs)if(kind!="velocity_feedback"||v.toObject()["quantity"]==QJsonValue("velocity"))selected.append(v.toObject()["id"]);
    const int p=selected.size(),free=compiledFreeDofs();int modes=recipe.value("design").toObject().contains("modeIndices")?recipe.value("design").toObject()["modeIndices"].toArray().size():free;int nx=2*modes;
    if(kind!="none"&&(m<=0||free<=0||(kind!="external"&&nx>128))){status->setText(tr("First provide a valid supported source and constraints. The interactive controller matrix editor supports up to 128 states."));return;}
    QJsonObject c{{"kind",kind}};
    if(kind!="none"){
        c["stateSource"]=stateSource;c["controlEveryFESteps"]=previous.value("controlEveryFESteps").toInt(1);c["selectedOutputs"]=selected;
        c["voltageLower"]=zeros(m,-100);c["voltageUpper"]=zeros(m,100);c["deltaVoltageLimits"]=zeros(m,10);
        if(kind!="LQR"&&kind!="MPC"&&kind!="external"){c["reference"]=zeros(p);c["bias"]=zeros(m);}
        if(kind=="external"){c["stateSource"]="sensor_only";c["external"]=previous.value("external").isObject()?previous.value("external"):QJsonValue(QJsonObject{{"connection",QJsonObject{{"transport","stdio"},{"executable",""},{"args",QJsonArray{}},{"algorithm",QJsonObject{{"id","user-external"},{"version","1"}}},{"requestTimeout",5.},{"handshakeTimeout",30.}}},{"initialState",QJsonObject{}},{"seed",0}});}
        if(kind=="velocity_feedback")c["gain"]=diagonal(m,p,1);
        if(kind=="PI"||kind=="PD"||kind=="PID"){c["kp"]=diagonal(m,p,1);if(kind!="PD")c["ki"]=diagonal(m,p);if(kind!="PI"){c["kd"]=diagonal(m,p);c["derivativeFilterAlpha"]=.9;}}
        if(kind=="discrete_state_space"){
            int n=previous.value("Ac").toArray().isEmpty()?1:previous.value("Ac").toArray().size();c["Ac"]=diagonal(n,n);c["Bc"]=diagonal(n,p,1);c["Cc"]=diagonal(m,n,1);c["Dc"]=diagonal(m,p);c["initialState"]=zeros(n);
            c["samplingPeriod"]=recipe.value("dt").toDouble()*c.value("controlEveryFESteps").toInt();QJsonArray units;for(const auto &id:selected)for(const auto &v:outputs)if(v.toObject()["id"]==id)units.append(v.toObject()["unit"]);c["inputUnits"]=units;QJsonArray volts;for(int i=0;i<m;++i)volts.append("V");c["outputUnits"]=volts;
        }
        if(kind=="LQR"||kind=="MPC"){
            c["Q"]=diagonal(nx,nx,1);c["R"]=diagonal(m,m,1);c["stateReference"]=zeros(nx);
            if(kind=="MPC"){c["Qf"]=diagonal(nx,nx,1);c["horizon"]=10;c["verificationTolerance"]=1e-6;c["maxIterations"]=500;}
            if(stateSource=="sensor_only")c["observer"]=QJsonObject{{"processCovariance",diagonal(nx,nx,1e-6)},{"measurementCovariance",diagonal(p,p,1e-4)},{"initialPrior",zeros(nx)}};
        }
    }
    setValue({"controller"},c);queueEditorRebuild();
}

QStringList PiezoWorkbenchWidget::allowedForceDirections() const {
    if(recipe.value("family")==QJsonValue("axialRod"))return {"X","Y","Z"};
    if(recipe.value("family")==QJsonValue("solid3D"))return {"X","Y","Z","local:EX","local:EY","local:EZ"};
    const bool plate=recipe.value("family")==QJsonValue("flatPlate");const auto frame=recipe.value("frame").toObject();
    const auto ex=frame.value("ex").toArray(),ey=frame.value("ey").toArray(),ez=frame.value("ez").toArray();QStringList directions;
    if(ex.size()==3&&ey.size()==3&&ez.size()==3){
        const QStringList axes{"X","Y","Z"};
        for(int i=0;i<3;++i)if(plate||std::abs(ey[i].toDouble())<=1e-10)directions.append(axes[i]);
        for(int i=0;i<3;++i){const double missing=plate?std::abs(ez[i].toDouble()):std::hypot(ex[i].toDouble(),ez[i].toDouble());if(missing<=1e-10)directions.append("R"+axes[i]);}
    }
    directions.append(plate?QStringList{"local:EX","local:EY","local:EZ","local:RX","local:RY"}:QStringList{"local:EX","local:EZ","local:RY"});return directions;
}
QString PiezoWorkbenchWidget::defaultForceDirection() const {return recipe.value("family")==QJsonValue("axialRod")?"X":"local:EZ";}
bool PiezoWorkbenchWidget::forceDirectionsValid() const {
    const auto directions=allowedForceDirections();const auto valid=[&](const QJsonObject &channel){const auto parts=channel.value("channelId").toString().split(':');
        if((parts.size()!=3&&parts.size()!=4)||parts.value(0)!="node"||!directions.contains(forceDirection(channel.value("channelId").toString())))return false;
        bool ok=false;const double node=parts[1].toDouble(&ok);if(!ok)return false;bool found=false;for(const auto &entry:nodeCatalog)if(entry.toObject()["id"].toDouble()==node)found=true;
        return found&&channel.value("unit")==QJsonValue(forceUnit(forceDirection(channel.value("channelId").toString())));
    };
    const auto excitation=recipe.value("excitation").toObject();for(const auto &value:excitation.value("channels").toArray())if(!valid(value.toObject()))return false;
    for(const auto &value:excitation.value("externalFunctions").toArray())for(const auto &channel:value.toObject()["channels"].toArray())if(!valid(channel.toObject()))return false;
    return true;
}
QComboBox *PiezoWorkbenchWidget::forceDirectionEditor(const QString &channelId,QWidget *parent,const QString &name) {
    auto *combo=new QComboBox(parent);combo->setObjectName(name);for(const auto &direction:allowedForceDirections()){
        combo->addItem(direction.startsWith("local:")?tr("Local %1").arg(direction.section(':',-1)):tr("Global %1").arg(direction),direction);
    }
    const auto current=forceDirection(channelId);int index=combo->findData(current);
    if(index<0){combo->addItem(tr("Invalid direction (%1): edit required").arg(current),current);index=combo->count()-1;combo->setItemData(index,0,Qt::UserRole-1);}
    combo->setCurrentIndex(index);combo->setProperty("invalidForceDirection",!allowedForceDirections().contains(current));return combo;
}
void PiezoWorkbenchWidget::bindForceChannelEditors(const QStringList &path,QComboBox *node,QComboBox *direction,QLabel *returnedName,QLabel *unit) {
    const auto update=[this,path,node,direction,returnedName,unit]{const auto axis=direction->currentData().toString();auto channel=valueAt(path).toObject();
        const auto name=forceChannelId(node->currentData().toDouble(),axis),force=forceUnit(axis);channel["channelId"]=name;channel["unit"]=force;
        direction->setProperty("invalidForceDirection",!allowedForceDirections().contains(axis));setValue(path,channel);returnedName->setText(name);unit->setText(force);
    };connect(node,&QComboBox::currentIndexChanged,this,update);connect(direction,&QComboBox::currentIndexChanged,this,update);
}
void PiezoWorkbenchWidget::addExcitationEditor(QVBoxLayout *layout) {
    layout->addWidget(note(tr("Named nodal forces: zero, constant, sine, table or restricted time expression. Expressions use t in seconds and return the stated force unit."),this));
    const auto channels=recipe.value("excitation").toObject()["channels"].toArray();
    for(int i=0;i<channels.size();++i){auto path=QStringList{"excitation","channels",rowIndex(i)};const auto channel=channels[i].toObject();auto parts=channel.value("channelId").toString().split(':');
        auto *group=new QGroupBox(tr("Force channel %1 (%2)").arg(i+1).arg(channel.value("unit").toString()),this);auto *v=new QVBoxLayout(group);auto *binding=new QHBoxLayout;
        auto *node=new QComboBox(group);for(const auto &n:nodeCatalog)node->addItem(n.toObject()["label"].toString(),n.toObject()["id"].toDouble());node->setCurrentIndex(node->findData(parts.value(1).toDouble()));
        auto *dof=forceDirectionEditor(channel.value("channelId").toString(),group,QString("piezoForceDirection%1").arg(i));binding->addWidget(node);binding->addWidget(dof);v->addLayout(binding);
        auto *returnedName=new QLabel(channel.value("channelId").toString(),group),*unit=new QLabel(channel.value("unit").toString(),group);returnedName->setObjectName(QString("piezoForceReturnedName%1").arg(i));unit->setObjectName(QString("piezoForceUnit%1").arg(i));auto *names=new QFormLayout;names->addRow(tr("Force channel name"),returnedName);names->addRow(tr("Unit"),unit);v->addLayout(names);
        bindForceChannelEditors(path,node,dof,returnedName,unit);
        auto terms=channel.value("terms").toArray();
        for(int j=0;j<terms.size();++j){auto termPath=path+QStringList{"terms",rowIndex(j)};auto term=terms[j].toObject();auto *form=new QFormLayout;auto *kind=new QComboBox(group);kind->addItems({"constant","sine","table","expression","chirp","smoothPulse","multisine"});kind->setCurrentText(term.value("kind").toString());form->addRow(tr("Term %1").arg(j+1),kind);
            connect(kind,&QComboBox::currentIndexChanged,this,[this,termPath,kind]{const auto k=kind->currentText();QJsonObject t{{"kind",k}};
                if(k=="constant")t["value"]=0.;if(k=="sine")t={{"kind",k},{"amplitude",1.},{"frequencyHz",1.},{"phaseRad",0.}};
                if(k=="expression")t["expression"]="0";
                if(k=="table")t={{"kind",k},{"times",QJsonArray{0.,1.}},{"values",QJsonArray{0.,0.}},{"interpolation","linear"},{"outOfRange","zero"}};
                if(k=="chirp")t={{"kind",k},{"amplitude",1.},{"startTime",0.},{"duration",1.},{"startFrequencyHz",1.},{"endFrequencyHz",10.},{"phaseRad",0.}};
                if(k=="smoothPulse")t={{"kind",k},{"amplitude",1.},{"startTime",0.},{"riseTime",.1},{"holdTime",.2},{"fallTime",.1}};
                if(k=="multisine")t["components"]=QJsonArray{QJsonObject{{"amplitude",1.},{"frequencyHz",1.},{"phaseRad",0.}}};
                setValue(termPath,t);queueEditorRebuild();
            });
            for(auto it=term.begin();it!=term.end();++it){if(it.key()=="kind")continue;const auto key=it.key();
                if(it.value().isDouble())form->addRow(key,numberEditor(termPath+QStringList{key},group));
                else if(key=="expression"){auto *expression=new QLineEdit(it.value().toString(),group);expression->setObjectName("piezoForceExpression");connect(expression,&QLineEdit::textChanged,this,[this,termPath,key](const QString &s){setValue(termPath+QStringList{key},s);});form->addRow(tr("f(t)"),expression);}
                else if(key=="outOfRange")form->addRow(tr("Outside table"),choiceEditor(termPath+QStringList{key},{"zero","hold","error"},group));
            }v->addLayout(form);
            if(term.value("kind")==QJsonValue("table")){
                auto times=term.value("times").toArray(),values=term.value("values").toArray();auto *t=table({tr("Time (s)"),tr("Force")},group,"piezoForceTable");t->setRowCount(times.size());
                for(int r=0;r<times.size();++r){t->setCellWidget(r,0,numberEditor(termPath+QStringList{"times",rowIndex(r)},t));t->setCellWidget(r,1,numberEditor(termPath+QStringList{"values",rowIndex(r)},t));}v->addWidget(t);
                auto *add=new QPushButton(tr("Add force sample"),group);v->addWidget(add);connect(add,&QPushButton::clicked,this,[this,termPath]{auto ts=valueAt(termPath+QStringList{"times"}).toArray(),vs=valueAt(termPath+QStringList{"values"}).toArray();ts.append(ts.isEmpty()?0:ts.last().toDouble()+recipe.value("dt").toDouble());vs.append(0.);setValue(termPath+QStringList{"times"},ts);setValue(termPath+QStringList{"values"},vs);queueEditorRebuild();});
            }
            if(term.value("kind")==QJsonValue("multisine")){
                auto components=term.value("components").toArray();for(int c=0;c<components.size();++c){auto *f=new QFormLayout;for(const auto &key:{"amplitude","frequencyHz","phaseRad"})f->addRow(QString(key),numberEditor(termPath+QStringList{"components",rowIndex(c),key},group));v->addLayout(f);}
            }
        }
        auto *remove=new QPushButton(tr("Remove this force channel"),group);v->addWidget(remove);connect(remove,&QPushButton::clicked,this,[this,i]{auto a=recipe.value("excitation").toObject()["channels"].toArray();a.removeAt(i);setValue({"excitation","channels"},a);queueEditorRebuild();});layout->addWidget(group);
    }
    auto *add=new QPushButton(tr("Add nodal force channel"),this);add->setObjectName("piezoAddForce");layout->addWidget(add);
    connect(add,&QPushButton::clicked,this,[this]{if(nodeCatalog.isEmpty())return;auto a=recipe.value("excitation").toObject()["channels"].toArray();auto id=nodeCatalog.last().toObject()["id"].toDouble();const auto direction=defaultForceDirection();a.append(QJsonObject{{"channelId",forceChannelId(id,direction)},{"unit",forceUnit(direction)},{"terms",QJsonArray{QJsonObject{{"kind","constant"},{"value",0.}}}}});setValue({"excitation","channels"},a);queueEditorRebuild();});
    addExternalFunctionEditor(layout);
}

bool PiezoWorkbenchWidget::externalSourcesCurrent() const {
    const auto functions=recipe.value("excitation").toObject()["externalFunctions"].toArray();QJsonObject stamps;
    const auto fileStamp=[](const QFileInfo &info)->QJsonObject{
        const auto modified=info.lastModified();
        return {{"kind",info.isDir()?QStringLiteral("directory"):QStringLiteral("file")},
                {"canonicalPath",info.canonicalFilePath()},
                {"size",QString::number(info.size())},
                {"modifiedMs",QString::number(modified.toMSecsSinceEpoch())}};
    };
    const auto verifyFile=[&](const QString &path,const QString &expectedSha)->bool{
        QFileInfo before(path);
        if(path.isEmpty()||!before.exists()||!before.isFile()||before.canonicalFilePath().isEmpty()||!before.lastModified().isValid())return false;
        const auto beforeStamp=fileStamp(before);
        if(fileDigest(path)!=expectedSha)return false;
        QFileInfo after(path);
        if(!after.exists()||!after.isFile()||after.canonicalFilePath().isEmpty()||!after.lastModified().isValid()||
           beforeStamp!=fileStamp(after))return false;
        stamps.insert(after.canonicalFilePath(),beforeStamp);return true;
    };
    try {
        for(const auto &connection:externalProgramConnections(recipe)){
            const auto frozen=connection.value("expectedIdentity").toObject();if(frozen.isEmpty())throw std::runtime_error("Reload and freeze the external program identity");
            for(const auto &v:frozen.value("files").toArray()){auto f=v.toObject();if(!verifyFile(f.value("path").toString(),f.value("sha256").toString()))throw std::runtime_error("External program or dependency changed");}
        }
        for(const auto &value:functions){
            const auto function=value.toObject();
            if(!verifyFile(function.value("sourcePath").toString(),function.value("sourceSha256").toString()))throw std::runtime_error("external force source changed");
            for(const auto &dependency:function.value("dependencyFiles").toArray()){
                const auto file=dependency.toObject();
                if(!verifyFile(file.value("path").toString(),file.value("sha256").toString()))throw std::runtime_error("external force dependency changed");
            }
        }
        externalSourceMetadataFunctions=functions;externalSourceMetadataStamps=stamps;externalSourceMetadataReady=true;
        return true;
    }catch(...){externalSourceMetadataFunctions={};externalSourceMetadataStamps={};externalSourceMetadataReady=false;return false;}
}
bool PiezoWorkbenchWidget::externalSourcesMetadataCurrent() const {
    const auto functions=recipe.value("excitation").toObject()["externalFunctions"].toArray();
    if(functions.isEmpty()&&externalProgramConnections(recipe).isEmpty())return true;
    if(!externalSourceMetadataReady||functions!=externalSourceMetadataFunctions)return false;
    QJsonObject stamps;
    const auto fileStamp=[](const QFileInfo &info)->QJsonObject{
        const auto modified=info.lastModified();
        return {{"kind",info.isDir()?QStringLiteral("directory"):QStringLiteral("file")},
                {"canonicalPath",info.canonicalFilePath()},
                {"size",QString::number(info.size())},
                {"modifiedMs",QString::number(modified.toMSecsSinceEpoch())}};
    };
    const auto addPath=[&](const QString &path)->bool{
        QFileInfo info(path);
        if(path.isEmpty()||!info.exists()||!info.isFile()||info.canonicalFilePath().isEmpty()||!info.lastModified().isValid())return false;
        stamps.insert(info.canonicalFilePath(),fileStamp(info));return true;
    };
    for(const auto &connection:externalProgramConnections(recipe)){
        const auto frozen=connection.value("expectedIdentity").toObject();if(frozen.isEmpty())return false;
        for(const auto &v:frozen.value("files").toArray())if(!addPath(v.toObject().value("path").toString()))return false;
    }
    for(const auto &value:functions){
        const auto function=value.toObject();
        if(!addPath(function.value("sourcePath").toString()))return false;
        for(const auto &dependency:function.value("dependencyFiles").toArray())
            if(!addPath(dependency.toObject().value("path").toString()))return false;
    }
    return stamps==externalSourceMetadataStamps;
}
bool PiezoWorkbenchWidget::addExternalForceFunctionFrom(const QString &path,const QString &functionName,const QStringList &dependencies) {
    if(recipe.isEmpty()||nodeCatalog.isEmpty())return false;
    try {
        const auto axis=defaultForceDirection();
        auto function=freezeFunction(QJsonObject{{"sourcePath",path},{"functionName",functionName},{"missingPolicy","error"},{"cacheExactTime",false},
            {"channels",QJsonArray{QJsonObject{{"channelId",forceChannelId(nodeCatalog.last().toObject()["id"].toDouble(),axis)},{"unit",forceUnit(axis)}}}}},dependencies);
        auto functions=recipe.value("excitation").toObject()["externalFunctions"].toArray();functions.append(function);
        setValue({"excitation","externalFunctions"},functions);queueEditorRebuild();return true;
    }catch(const std::exception &e){invalidatePreview(QString::fromUtf8(e.what()));return false;}
}
bool PiezoWorkbenchWidget::reloadExternalForceFunction(int index,const QStringList &dependencies) {
    auto functions=recipe.value("excitation").toObject()["externalFunctions"].toArray();if(index<0||index>=functions.size())return false;
    // Explicit user action replaces frozen identities; never refresh them during preview.
    invalidatePreview(tr("Reloading external force source and dependencies..."));
    try {functions[index]=freezeFunction(functions[index].toObject(),dependencies);setValue({"excitation","externalFunctions"},functions);queueEditorRebuild();
        status->setText(tr("External force files and environment reloaded. Preview and confirm the complete experiment again."));return true;
    }catch(const std::exception &e){status->setText(QString::fromUtf8(e.what()));return false;}
}
void PiezoWorkbenchWidget::addExternalFunctionEditor(QVBoxLayout *layout) {
    layout->addWidget(note(tr("Trusted Python force functions receive only t in seconds and return named forces with units. Choose actual nodes and DOFs below. Built-in terms and external functions on the same channel add together. Changed files require an explicit reload and a new whole preview."),this));
    const auto functions=recipe.value("excitation").toObject()["externalFunctions"].toArray();
    for(int i=0;i<functions.size();++i){auto path=QStringList{"excitation","externalFunctions",rowIndex(i)};const auto function=functions[i].toObject();auto *group=new QGroupBox(tr("External force function %1").arg(i+1),this);auto *v=new QVBoxLayout(group);auto *form=new QFormLayout;
        form->addRow(tr("Source file"),note(function.value("sourcePath").toString(),group));
        auto *name=new QLineEdit(function.value("functionName").toString(),group);name->setObjectName(QString("piezoExternalFunctionName%1").arg(i));connect(name,&QLineEdit::textChanged,this,[this,path](const QString &text){setValue(path+QStringList{"functionName"},text);});form->addRow(tr("Function name"),name);
        auto *policy=choiceEditor(path+QStringList{"missingPolicy"},{"error","zero"},group,QString("piezoExternalMissingPolicy%1").arg(i));form->addRow(tr("Missing channel"),policy);
        auto *cache=new QCheckBox(tr("Reuse results at exactly the same time"),group);cache->setObjectName(QString("piezoExternalCache%1").arg(i));cache->setChecked(function.value("cacheExactTime").toBool());connect(cache,&QCheckBox::toggled,this,[this,path](bool checked){setValue(path+QStringList{"cacheExactTime"},checked);});form->addRow(cache);v->addLayout(form);
        const auto environment=function.value("environment").toObject();v->addWidget(note(tr("Frozen source SHA-256: %1\nPython %2; NumPy %3; SciPy %4\nDependencies: %5").arg(function.value("sourceSha256").toString()).arg(environment.value("pythonVersion").toString()).arg(environment.value("numpyVersion").toString()).arg(environment.value("scipyVersion").toString()).arg(functionDependencies(function).join("\n")),group));
        auto *channels=table({tr("Bound node"),tr("Physical DOF"),tr("Returned force name"),tr("Unit")},group,QString("piezoExternalChannels%1").arg(i));auto array=function.value("channels").toArray();channels->setRowCount(array.size());
        for(int j=0;j<array.size();++j){auto channelPath=path+QStringList{"channels",rowIndex(j)};const auto channel=array[j].toObject();auto parts=channel.value("channelId").toString().split(':');
            auto *node=new QComboBox(channels);for(const auto &n:nodeCatalog)node->addItem(n.toObject()["label"].toString(),n.toObject()["id"].toDouble());node->setCurrentIndex(node->findData(parts.value(1).toDouble()));
            auto *dof=forceDirectionEditor(channel.value("channelId").toString(),channels,QString("piezoExternalForceDirection%1_%2").arg(i).arg(j));
            auto *label=new QLabel(channel.value("channelId").toString(),channels),*unit=new QLabel(channel.value("unit").toString(),channels);channels->setCellWidget(j,0,node);channels->setCellWidget(j,1,dof);channels->setCellWidget(j,2,label);channels->setCellWidget(j,3,unit);
            bindForceChannelEditors(channelPath,node,dof,label,unit);
        }v->addWidget(channels);
        auto *buttons=new QHBoxLayout;auto *addChannel=new QPushButton(tr("Add returned force channel"),group),*removeChannel=new QPushButton(tr("Remove selected channel"),group);buttons->addWidget(addChannel);buttons->addWidget(removeChannel);v->addLayout(buttons);
        connect(addChannel,&QPushButton::clicked,this,[this,path]{if(nodeCatalog.isEmpty())return;auto a=valueAt(path+QStringList{"channels"}).toArray();const auto direction=defaultForceDirection();a.append(QJsonObject{{"channelId",forceChannelId(nodeCatalog.last().toObject()["id"].toDouble(),direction)},{"unit",forceUnit(direction)}});setValue(path+QStringList{"channels"},a);queueEditorRebuild();});
        connect(removeChannel,&QPushButton::clicked,this,[this,path,channels]{auto a=valueAt(path+QStringList{"channels"}).toArray();const int selected=channels->currentRow();if(selected>=0&&selected<a.size()){a.removeAt(selected);setValue(path+QStringList{"channels"},a);queueEditorRebuild();}});
        auto *reload=new QPushButton(tr("Reload source / dependencies and freeze current environment"),group);reload->setObjectName(QString("piezoExternalReload%1").arg(i));v->addWidget(reload);connect(reload,&QPushButton::clicked,this,[this,i]{reloadExternalForceFunction(i,functionDependencies(recipe.value("excitation").toObject()["externalFunctions"].toArray()[i].toObject()));});
        auto *deps=new QPushButton(tr("Choose dependency files and reload…"),group);deps->setObjectName(QString("piezoExternalDependencies%1").arg(i));v->addWidget(deps);connect(deps,&QPushButton::clicked,this,[this,i]{const auto files=QFileDialog::getOpenFileNames(this,tr("Select all declared force function dependencies"));if(!files.isEmpty())reloadExternalForceFunction(i,files);});
        if(!function.value("dependencyFiles").toArray().isEmpty()){auto *clear=new QPushButton(tr("Clear declared dependencies and reload"),group);v->addWidget(clear);connect(clear,&QPushButton::clicked,this,[this,i]{reloadExternalForceFunction(i,{});});}
        auto *remove=new QPushButton(tr("Remove external force function"),group);v->addWidget(remove);connect(remove,&QPushButton::clicked,this,[this,i]{auto a=recipe.value("excitation").toObject()["externalFunctions"].toArray();a.removeAt(i);setValue({"excitation","externalFunctions"},a);queueEditorRebuild();});layout->addWidget(group);
    }
    auto *add=new QPushButton(tr("Choose external Python force function…"),this);add->setObjectName("piezoAddExternalFunction");layout->addWidget(add);connect(add,&QPushButton::clicked,this,[this]{auto path=QFileDialog::getOpenFileName(this,tr("Select trusted time force function"),{},tr("Python force source (*.py)"));if(!path.isEmpty())addExternalForceFunctionFrom(path,"force");});
}

void PiezoWorkbenchWidget::addExternalProgramEditor(QVBoxLayout *layout,const QStringList &path,const QString &role) {
    auto *group=new QGroupBox(role=="controller"?tr("External controller connection"):tr("External excitation connection"),this);auto *v=new QVBoxLayout(group);auto *form=new QFormLayout;
    const auto connectionPath=path+QStringList{"connection"};const auto config=valueAt(connectionPath).toObject();
    auto *transport=new QComboBox(group);transport->setObjectName(role+"ExternalTransport");transport->addItem(tr("Start external program"),"stdio");transport->addItem(tr("Connect to localhost TCP"),"tcp");transport->setCurrentIndex(config.value("transport")==QJsonValue("tcp")?1:0);form->addRow(tr("Connection"),transport);
    connect(transport,&QComboBox::currentIndexChanged,this,[this,connectionPath,transport]{auto c=valueAt(connectionPath).toObject();c.remove("expectedIdentity");c["transport"]=transport->currentData().toString();if(transport->currentData()==QVariant("tcp")){c.remove("executable");c.remove("args");c.remove("cwd");c["host"]="127.0.0.1";c["port"]=5555;}else{c.remove("host");c.remove("port");c["executable"]="";c["args"]=QJsonArray{};}setValue(connectionPath,c);queueEditorRebuild();});
    const auto textField=[this,group,form,connectionPath](const QString &label,const QStringList &suffix,const QString &value){auto *edit=new QLineEdit(value,group);edit->setObjectName(piezoEditorObjectName(connectionPath+suffix));form->addRow(label,edit);connect(edit,&QLineEdit::editingFinished,this,[this,connectionPath,suffix,edit]{setValue(connectionPath+suffix,edit->text());});return edit;};
    if(config.value("transport")==QJsonValue("tcp")){
        textField(tr("Local host"),{"host"},config.value("host").toString("127.0.0.1"));auto *port=new QSpinBox(group);port->setRange(1,65535);port->setValue(config.value("port").toInt(5555));form->addRow(tr("Port"),port);connect(port,&QSpinBox::valueChanged,this,[this,connectionPath](int n){setValue(connectionPath+QStringList{"port"},n);});
    }else{
        auto *program=textField(tr("Executable"),{"executable"},config.value("executable").toString());auto *choose=new QPushButton(tr("Choose executable…"),group);form->addRow(choose);connect(choose,&QPushButton::clicked,this,[this,connectionPath,program]{auto file=QFileDialog::getOpenFileName(this,tr("Choose external executable"));if(file.isEmpty())return;program->setText(file);setValue(connectionPath+QStringList{"executable"},file);});
        auto *args=new QPlainTextEdit(group);args->setObjectName(role+"ExternalArguments");args->setMaximumHeight(85);QStringList lines;for(const auto &arg:config.value("args").toArray())lines.append(arg.toString());args->setPlainText(lines.join('\n'));form->addRow(tr("Arguments (one per line)"),args);connect(args,&QPlainTextEdit::textChanged,this,[this,connectionPath,args]{QJsonArray a;for(const auto &line:args->toPlainText().split('\n',Qt::SkipEmptyParts))a.append(line);setValue(connectionPath+QStringList{"args"},a);});
        textField(tr("Working directory"),{"cwd"},config.value("cwd").toString(QDir::currentPath()));
    }
    const auto algorithm=config.value("algorithm").toObject();textField(tr("Algorithm ID"),{"algorithm","id"},algorithm.value("id").toString());textField(tr("Algorithm version"),{"algorithm","version"},algorithm.value("version").toString());
    auto *timeout=new ScientificDoubleSpinBox(group);timeout->setRange(.01,50.);timeout->setValue(config.value("requestTimeout").toDouble(5.));form->addRow(tr("Response timeout (s)"),timeout);connect(timeout,&QDoubleSpinBox::valueChanged,this,[this,connectionPath](double t){setValue(connectionPath+QStringList{"requestTimeout"},t);});v->addLayout(form);
    auto *dependencies=new QPushButton(tr("Choose algorithm / weights / dependency files…"),group);v->addWidget(dependencies);connect(dependencies,&QPushButton::clicked,this,[this,connectionPath]{auto paths=QFileDialog::getOpenFileNames(this,tr("Declare all external algorithm dependencies"));if(paths.isEmpty())return;setValue(connectionPath+QStringList{"dependencies"},QJsonArray::fromStringList(paths));});
    auto *initial=new QPlainTextEdit(group);initial->setObjectName(role+"ExternalInitialState");initial->setMaximumHeight(65);initial->setPlainText(QString::fromUtf8(QJsonDocument(valueAt(path+QStringList{"initialState"}).toObject()).toJson(QJsonDocument::Compact)));v->addWidget(note(tr("Initial algorithm state (JSON object)"),group));v->addWidget(initial);
    connect(initial,&QPlainTextEdit::textChanged,this,[this,path,initial]{QJsonParseError error;const auto doc=QJsonDocument::fromJson(initial->toPlainText().toUtf8(),&error);if(error.error!=QJsonParseError::NoError||!doc.isObject()){invalidatePreview(tr("External initial state must be a valid JSON object."));return;}setValue(path+QStringList{"initialState"},doc.object());});
    auto *reload=new QPushButton(tr("Reload files and freeze external program identity"),group),*test=new QPushButton(tr("Test connection (handshake only)"),group);reload->setObjectName(role+"ExternalFreeze");test->setObjectName(role+"ExternalTest");v->addWidget(reload);v->addWidget(test);connect(reload,&QPushButton::clicked,this,[this,path]{inspectExternalConnection(path,false);});connect(test,&QPushButton::clicked,this,[this,path]{inspectExternalConnection(path,true);});
    const auto frozen=config.value("expectedIdentity").toObject();v->addWidget(note(frozen.isEmpty()?tr("Choose the program, arguments and dependencies, then freeze its identity before preview."):tr("Frozen identity: %1; %2 files. Program or dependency changes require reload and whole preview.").arg(frozen.value("identityTrust").toString()).arg(frozen.value("files").toArray().size()),group));layout->addWidget(group);
}

void PiezoWorkbenchWidget::addExternalExcitationEditors(QVBoxLayout *layout) {
    const auto sources=recipe.value("excitation").toObject().value("externalSources").toArray();
    for(int i=0;i<sources.size();++i){const auto path=QStringList{"excitation","externalSources",rowIndex(i)};auto source=sources[i].toObject();addExternalProgramEditor(layout,path,"excitation");
        auto *form=new QFormLayout;auto *stride=new QSpinBox(this);stride->setRange(1,1000000);stride->setValue(source.value("sampleEveryFESteps").toInt(1));form->addRow(tr("Sample excitation every N FE steps (held between samples)"),stride);connect(stride,&QSpinBox::valueChanged,this,[this,path](int n){setValue(path+QStringList{"sampleEveryFESteps"},n);});layout->addLayout(form);
        auto *channels=table({tr("External force output → model node / direction")},this,"piezoExternalProgramForces"+QString::number(i));const auto ids=source.value("channelIds").toArray();channels->setRowCount(ids.size());
        for(int j=0;j<ids.size();++j){auto *choice=new QComboBox(channels);for(const auto &n:nodeCatalog)for(const auto &axis:allowedForceDirections()){const auto node=n.toObject();choice->addItem(node.value("label").toString()+" / "+axis+" ("+forceUnit(axis)+")",forceChannelId(node.value("id").toDouble(),axis));}choice->setCurrentIndex(choice->findData(ids[j].toString()));channels->setCellWidget(j,0,choice);connect(choice,&QComboBox::currentIndexChanged,this,[this,path,j,choice]{setValue(path+QStringList{"channelIds",rowIndex(j)},choice->currentData().toString());});}layout->addWidget(channels);
        auto *add=new QPushButton(tr("Add external force output"),this),*removeChannel=new QPushButton(tr("Remove selected external force output"),this),*remove=new QPushButton(tr("Remove external excitation program"),this);layout->addWidget(add);layout->addWidget(removeChannel);layout->addWidget(remove);
        connect(add,&QPushButton::clicked,this,[this,path]{if(nodeCatalog.isEmpty())return;auto ids=valueAt(path+QStringList{"channelIds"}).toArray();ids.append(forceChannelId(nodeCatalog.last().toObject().value("id").toDouble(),defaultForceDirection()));setValue(path+QStringList{"channelIds"},ids);queueEditorRebuild();});
        connect(removeChannel,&QPushButton::clicked,this,[this,path,channels]{auto ids=valueAt(path+QStringList{"channelIds"}).toArray();if(channels->currentRow()<0)return;ids.removeAt(channels->currentRow());setValue(path+QStringList{"channelIds"},ids);queueEditorRebuild();});
        connect(remove,&QPushButton::clicked,this,[this,i]{auto sources=recipe.value("excitation").toObject().value("externalSources").toArray();sources.removeAt(i);setValue({"excitation","externalSources"},sources);queueEditorRebuild();});
    }
    auto *add=new QPushButton(tr("Add external excitation program / TCP connection"),this);add->setObjectName("piezoAddExternalProgram");layout->addWidget(add);connect(add,&QPushButton::clicked,this,[this]{if(nodeCatalog.isEmpty())return;auto sources=recipe.value("excitation").toObject().value("externalSources").toArray();sources.append(QJsonObject{{"connection",QJsonObject{{"transport","stdio"},{"executable",""},{"args",QJsonArray{}},{"algorithm",QJsonObject{{"id","user-external"},{"version","1"}}},{"requestTimeout",5.},{"handshakeTimeout",30.}}},{"channelIds",QJsonArray{forceChannelId(nodeCatalog.last().toObject().value("id").toDouble(),defaultForceDirection())}},{"sampleEveryFESteps",1},{"initialState",QJsonObject{}},{"seed",0}});setValue({"excitation","externalSources"},sources);queueEditorRebuild();});
}

void PiezoWorkbenchWidget::inspectExternalConnection(const QStringList &path,bool test) {
    const auto connectionPath=path+QStringList{"connection"};const auto connection=valueAt(connectionPath).toObject();auto input=connection;
    std::vector<std::pair<QStringList,QString>> initialStateTargets;
    if(!test)input.remove("expectedIdentity");
    if(test)try {
        QJsonArray ports;int seed=-1;
        const auto addPort=[&](const QString &id,const QString &role,const QString &direction,const QString &unit,
                               const QString &reference,const QJsonObject &binding,double initial,const QString &coordinateSystem=QStringLiteral("global")){
            const QMap<QString,QString> quantities{{"N","force"},{"N m","moment"},{"m","displacement"},{"m/s","velocity"},{"m/s^2","acceleration"},{"rad","rotation"},{"rad/s","angularVelocity"},{"rad/s^2","angularAcceleration"},{"V","voltage"},{"C","charge"}};
            if(!quantities.contains(unit))throw std::runtime_error("Unsupported external port unit: "+unit.toStdString());
            ports.append(QJsonObject{{"id",id},{"role",role},{"direction",direction},{"quantity",quantities.value(unit)},
                {"unit",unit},{"reference",reference},{"binding",binding},{"initialValue",initial},
                {"coordinateSystem",coordinateSystem},{"positiveDirection","+"}});
        };
        const auto controller=recipe.value("controller").toObject(),controllerExternal=controller.value("external").toObject();
        if(controller.value("kind")==QJsonValue("external")&&controllerExternal.value("connection").toObject()==connection){
            seed=controllerExternal.value("seed").toInt();initialStateTargets.push_back({{"controller","external"},"controller"});
            const auto available=feedbackCatalog(recipe),selected=controller.value("selectedOutputs").toArray();
            for(const auto &selectedId:selected){
                const auto found=std::find_if(available.begin(),available.end(),[&](const auto &value){return value.toObject().value("id")==selectedId;});
                if(found==available.end())throw std::runtime_error("External controller feedback selection is unavailable.");
                const auto item=found->toObject();const auto id=item.value("id").toString();
                addPort("readout:"+id,"controller","input",item.value("unit").toString(),"increment",{{"outputId",id}},0.);
            }
            for(const auto &value:recipe.value("ports").toArray()){
                const auto item=value.toObject();if(item.value("role")==QJsonValue("sensor"))continue;
                const auto id=item.value("portId").toString();addPort("actuator:"+id,"controller","output","V","absolute",{{"actuatorId",id}},item.value("initialValue").toDouble());
            }
            if(controller.value("stateSource")==QJsonValue("full_fe")){
                RModel source;if(!sourceSnapshot(session,&source))throw std::runtime_error("Select one source model before testing full FE feedback.");
                const auto candidate=RPiezoExperimentService::preview(source,recipe);if(!candidate.valid)throw std::runtime_error(candidate.message.toStdString());
                const auto package=recipe.value("family")==QJsonValue("axialRod")?PiezoAxialBackendBridge::create(candidate.candidate).exportPackage:PiezoNativeBackendBridge::create(candidate.candidate).exportPackage;
                for(const auto &dofValue:package.value("dofs").toArray()){
                    const auto pair=dofValue.toArray();if(pair.size()!=2)throw std::runtime_error("Native FE state port metadata is invalid.");
                    const auto id=pair[0].toString(),unit=pair[1].toString();
                    addPort("state:"+id+":q","controller","input",unit,"increment",{{"dofId",id}},0.);
                    addPort("state:"+id+":v","controller","input",unit=="m"?"m/s":"rad/s","increment",{{"dofId",id}},0.);
                }
            }
        }
        const auto sources=recipe.value("excitation").toObject().value("externalSources").toArray();
        for(int i=0;i<sources.size();++i){
            const auto source=sources[i].toObject();if(source.value("connection").toObject()!=connection)continue;
            const int sourceSeed=source.value("seed").toInt();if(seed<0)seed=sourceSeed;else if(seed!=sourceSeed)throw std::runtime_error("Shared external roles must use the same seed.");
            initialStateTargets.push_back({{"excitation","externalSources",rowIndex(i)},"excitation"});
            for(const auto &channelValue:source.value("channelIds").toArray()){
                const auto id=channelValue.toString();const auto parts=id.split(':');if((parts.size()!=3&&parts.size()!=4)||parts[0]!="node")throw std::runtime_error("External force channel ID is invalid.");
                const bool local=parts.size()==4;const QString dof=local?parts[3]:parts[2],unit=forceUnit(id);
                addPort("force:"+id,"excitation","output",unit,"absolute",{{"nodeId",parts[1]},{"dof",dof}},0.,local?"local":"global");
            }
        }
        if(ports.isEmpty()||seed<0)throw std::runtime_error("No external roles use this connection.");
        input=QJsonObject{{"connection",connection},{"ports",ports},{"seed",seed}};
    }catch(const std::exception &error){status->setText(QString::fromUtf8(error.what()));return;}
    const auto runtime=PiezoNumericTransport::defaultRuntime();auto file=std::make_shared<QTemporaryFile>();if(!file->open()){status->setText(tr("Cannot create external connection test input."));return;}file->write(QJsonDocument(input).toJson(QJsonDocument::Compact));file->flush();
    auto *process=new QProcess(this);process->setProgram(runtime.first);process->setArguments({"-I","-B",QDir(runtime.second).filePath("range_workflow/piezo_external_io.py"),test?"--test":"--inspect",file->fileName()});auto output=std::make_shared<QByteArray>();const auto generation=contextGeneration;
    auto *timer=new QTimer(process);timer->setSingleShot(true);timer->setInterval(55000);connect(timer,&QTimer::timeout,process,&QProcess::kill);connect(this,&QObject::destroyed,process,[process]{process->kill();});
    connect(process,&QProcess::readyReadStandardOutput,this,[process,output]{*output+=process->readAllStandardOutput();if(output->size()>2*1024*1024)process->kill();});
    connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error){if(error==QProcess::FailedToStart){status->setText(tr("External connection helper could not start: %1").arg(process->errorString()));process->deleteLater();}});
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,path,connectionPath,connection,test,generation,process,output,file,initialStateTargets](int code,QProcess::ExitStatus exit){
        *output+=process->readAllStandardOutput();const auto result=QJsonDocument::fromJson(*output).object();if(generation==contextGeneration&&valueAt(connectionPath).toObject()==connection){if(exit!=QProcess::NormalExit||code||!result.value("ok").toBool())status->setText(tr("External connection failed: %1").arg(result.value("error").toString(QString::fromUtf8(process->readAllStandardError().right(4096)))));else if(test){const auto initial=result.value("handshake").toObject().value("initialState").toObject();for(const auto &target:initialStateTargets){const auto state=initial.value(target.second);if(!state.isObject())continue;auto owner=valueAt(target.first).toObject();owner["initialState"]=state;setValue(target.first,owner);}if(!initialStateTargets.empty())queueEditorRebuild();status->setText(tr("Combined external roles passed handshake and exact port-map validation. Initial states loaded; algorithms were not advanced."));}else{auto normalized=result.value("connection").toObject();normalized["expectedIdentity"]=result.value("identity");setValue(connectionPath,normalized);queueEditorRebuild();status->setText(tr("External program identity frozen. Preview and confirm the complete experiment."));}}process->deleteLater();
    });status->setText(test?tr("Testing external handshake…"):tr("Inspecting external program files…"));process->start();timer->start();
}

void PiezoWorkbenchWidget::previewExperiment() {
    if(!templateSynchronized){invalidatePreview(tr("Template mesh/layers are not synchronized. Correct template values and retry updating the template."));return;}
    if(recipe.value("damping").toObject()["kind"]==QJsonValue("perCluster")&&computedModes.isEmpty()){
        invalidatePreview(tr("Compute actual mode clusters for this draft before previewing per-cluster damping."));return;
    }
    for(auto *grid:findChildren<QTableWidget*>())if(grid->property("invalidNumericEntry").toBool()){invalidatePreview(tr("Correct invalid numeric matrix entries before preview."));return;}
    requestPreview("piezo.configure-experiment",recipe);
}
void PiezoWorkbenchWidget::computeModes() {
    invalidatePreview(tr("Computing full modes and clusters with the production backend..."));
    if(!externalSourcesCurrent()){status->setText(tr("External force files changed. Reload them explicitly before computing modes."));return;}
    if(!templateSynchronized){status->setText(tr("Correct template synchronization before computing modes."));return;}
    RModel source;if(!sourceSnapshot(session,&source)){status->setText(tr("Select one available source document."));return;}
    try{
        const auto input=modalInputRecipe(recipe);const auto candidate=RPiezoExperimentService::preview(source,input);
        if(!candidate.valid){status->setText(candidate.message);return;}
        const auto package=recipe.value("family")==QJsonValue("axialRod")?PiezoAxialBackendBridge::create(candidate.candidate).exportPackage:PiezoNativeBackendBridge::create(candidate.candidate).exportPackage;const auto design=package.value("design").toObject();
        const auto omega=design.value("omega").toArray(),clusters=design.value("clusters").toArray();
        if(omega.isEmpty()||clusters.isEmpty()||omega.size()!=candidate.report.value("freeDofs").toInt())throw std::runtime_error("Production modal report does not cover every free FE coordinate");
        int next=0;QJsonArray ends;
        for(const auto &cluster:clusters){const auto indices=cluster.toArray();if(indices.isEmpty())throw std::runtime_error("Empty production mode cluster");
            for(const auto &index:indices)if(index.toInt(-1)!=next++)throw std::runtime_error("Production clusters do not provide a complete ordered eigensystem");ends.append(next);
        }
        if(next!=omega.size())throw std::runtime_error("Production cluster coverage differs from the eigensystem");
        computedModes={};for(const auto &key:{"selectedModes","omega","clusters","modalResiduals","massOrthogonalityError"})computedModes[key]=design[key];computedModesInput=input;
        if(recipe.value("damping").toObject()["kind"]==QJsonValue("perCluster")){
            auto damping=recipe.value("damping").toObject();if(damping.value("clusterEnds")!=QJsonValue(ends)||damping.value("ratios").toArray().size()!=ends.size())damping["ratios"]=zeros(int(ends.size()),.01);
            damping["clusterEnds"]=ends;setValue({"damping"},damping);
        }
        queueEditorRebuild();status->setText(tr("Actual full FE frequencies, complete clusters and numerical qualification computed. Whole experiment confirmation is still required."));
    }catch(const std::exception &e){computedModes={};computedModesInput={};status->setText(QString::fromUtf8(e.what()));}
    catch(...){computedModes={};computedModesInput={};status->setText(tr("Production modal computation rejected this source."));}
}
bool PiezoWorkbenchWidget::savedSourceSnapshot(QJsonObject *document,RModel *model,bool *sourceBusy) {
    if(sourceBusy)*sourceBusy=false;
    // context.query also uses the session lock. Detect transient contention
    // before it can turn a retryable source into an empty document result.
    if(session){
        if(!session->trylock(0)){
            if(sourceBusy)*sourceBusy=true;
            status->setText(tr("Source is busy. Retry when it is available."));
            return false;
        }
        session->unlock();
    }
    if(!externalSourcesCurrent()){invalidatePreview(tr("External force files changed. Reload them explicitly and confirm the experiment before export."));return false;}
    bool contextUnavailable=false;
    *document=currentDocument(&contextUnavailable);
    if(document->isEmpty()){
        if(sourceBusy)*sourceBusy=contextUnavailable;
        // The lock can be taken between the preflight above and context.query.
        if(sourceBusy&&session){
            if(!session->trylock(0))*sourceBusy=true;
            else session->unlock();
        }
        status->setText(tr("The saved source is busy or unavailable. Retry when it is available."));return false;
    }
    if(savedRecipe.isEmpty()||recipe!=savedRecipe){status->setText(tr("Apply the complete draft before exporting the current bound experiment."));return false;}
    for(const auto &key:{"documentId","sessionEpoch","documentRevision","project","draft"})if(document->value(key)!=loadedDocument.value(key)){invalidatePreview(tr("Source changed. Refresh the current saved experiment before export."));return false;}
    if(!sourceSnapshot(session,model,sourceBusy)){status->setText(tr("Source is busy or unavailable. Retry when it is available."));return false;}
    return true;
}
QString PiezoWorkbenchWidget::exportDirectory() const {
    if(session&&session->getSelectedModelIDs().size()==1){
        // A registry record supplies a chooser location, never a user-entered
        // native identity. The artifact service certifies selected record bytes.
        const auto jobs=SolverManager::getInstance().jobsQuery(0,200)["jobs"].toArray();
        for(int i=int(jobs.size())-1;i>=0;--i){const auto job=jobs[i].toObject();
            if(job.value("sourceRuntimeStamp").toObject()["documentId"]!=loadedDocument.value("documentId"))continue;
            const auto path=job.value("resultArtifactRef").toString();if(!path.isEmpty()&&QFileInfo(path).exists())return QFileInfo(path).absolutePath();
        }
        if(session->trylock()){const auto ids=session->getSelectedModelIDs();QString path;if(ids.size()==1)path=session->getModel(ids.first()).getFileName();session->unlock();if(!path.isEmpty())return QFileInfo(path).absolutePath();}
    }
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}
void PiezoWorkbenchWidget::exportMatrixPackage() {
    QJsonObject document;RModel model;if(!savedSourceSnapshot(&document,&model))return;
    const auto path=QFileDialog::getSaveFileName(this,tr("Export actual FE / ROM package"),QDir(exportDirectory()).filePath("piezo-matrices.json"),tr("FE / ROM matrix package (*.json)"));
    if(!path.isEmpty())exportMatrixPackageTo(path);
}
bool PiezoWorkbenchWidget::exportMatrixPackageTo(const QString &path) {
    QJsonObject document;RModel model;if(!savedSourceSnapshot(&document,&model))return false;
    if(targetsSelectedDocument(session,path)){status->setText(tr("Choose a separate JSON file; the current model file is protected."));return false;}
    QString error;RPiezoArtifactService::Report report;
    if(!RPiezoArtifactService::writeMatrixPackage(model,path,&error,&report)){status->setText(error);return false;}
    status->setText(tr("Exported certified operators and FE / ROM design from the current bound experiment (SI units and coordinate mappings included)."));return true;
}
void PiezoWorkbenchWidget::exportAcceptedTimeSeries() {
    if(!acceptedSeriesCurrent()||acceptedSelectionPaths.isEmpty()){
        clearAcceptedSeries(tr("Open or wait for certified accepted results before exporting them."));return;
    }
    const auto path=QFileDialog::getSaveFileName(this,tr("Export certified accepted samples"),QDir(exportDirectory()).filePath("piezo-accepted.csv"),tr("Accepted samples (*.csv)"));
    if(!path.isEmpty())exportCurrentAcceptedTimeSeriesTo(path);
}
bool PiezoWorkbenchWidget::discoverAcceptedTrace(const QStringList &paths,const QString &fingerprint,PiezoResultSeries::TraceProof *proof,QStringList *files,QStringList *hashes) const {
    if(paths.isEmpty())return false;
    QDir directory(QFileInfo(paths.first()).absolutePath());
    if(directory.dirName()=="run-results"){requireResult(directory.cdUp(),tr("Accepted history directory is unavailable."));}
    const QString root=directory.absolutePath(),latest=directory.filePath("latest.json"),tracePath=directory.filePath("accepted-trace.rpt"),pending=directory.filePath("pending-publication.json");
    const bool hasAuthority=QFileInfo::exists(tracePath)||QFileInfo(tracePath).isSymLink()||QFileInfo::exists(pending)||QFileInfo(pending).isSymLink();
    if(!hasAuthority){if(!QFileInfo::exists(latest))return false;const auto pointer=resultObject(resultBytes(latest,64*1024));if(pointer.value("schema")!="range.native-checkpoint-pointer/v2")return false;}
    requireResult(paths.size()==1,tr("Select one compact native result; its accepted history is opened automatically."));
    requireResult(!QFileInfo::exists(pending)&&!QFileInfo(pending).isSymLink(),tr("Accepted history has unresolved PENDING publication authority."));safeResultPath(root);
    QJsonObject authority;QString inputPath;QJsonArray jobs;for(int offset=0;;){const auto page=SolverManager::getInstance().jobsQuery(offset,200);const auto rows=page.value("jobs").toArray();for(const auto &row:rows)jobs.append(row);if(!page.value("hasMore").toBool())break;requireResult(!rows.isEmpty()&&offset<=std::numeric_limits<int>::max()-rows.size(),tr("Accepted history registry pagination is invalid."));offset+=int(rows.size());}
    for(const auto &value:jobs){const auto job=value.toObject(),candidate=job.value("nativeResultProvenance").toObject().value("executionContext").toObject().value("nativeAcceptedTrace").toObject();
        if(candidate.isEmpty()||!sameResultPath(candidate.value("checkpointDirectory").toString(),root))continue;
        const auto artifact=job.value("resultArtifactRef").toString(),resolved=job.value("resultSourceBinding").toObject().value("resolvedSourcePath").toString();
        requireResult(sameResultPath(paths.first(),artifact)||(!resolved.isEmpty()&&sameResultPath(paths.first(),resolved)),tr("Selected result does not match the registered compact artifact."));
        requireResult(job.value("provenanceFrozen").toBool()&&authority.isEmpty(),tr("Compact accepted history requires one frozen registered execution context."));authority=candidate;
    }
    requireResult(!authority.isEmpty(),tr("Compact accepted history has no verified registered execution context. Reopen its owning project/job."));
    resultKeys(authority,{"checkpointDirectory","context","inputModelPath","inputModelSha256"});const auto context=authority.value("context").toObject();
    resultKeys(context,{"schema","sourceSha256","modelSha256","controlSha256","runtimeSha256","packageSha256","policy","maxAcceptedSteps","maxSimulationSeconds","dtS"});
    requireResult(context.value("schema")=="range.native-checkpoint-context/v1"&&context.value("policy")=="accepted-steps-or-sim-time-v1"&&context.value("maxAcceptedSteps")==10&&context.value("maxSimulationSeconds")==1.,tr("Unsupported accepted history execution context."));
    for(const auto key:{"sourceSha256","modelSha256","controlSha256","runtimeSha256","packageSha256"})resultSha(context.value(key));
    inputPath=authority.value("inputModelPath").toString();safeResultPath(inputPath);rawResultModel(inputPath);requireResult(resultSha(authority.value("inputModelSha256"))==resultSha(context.value("modelSha256"))&&fileDigest(inputPath)==context.value("modelSha256").toString(),tr("Original immutable MODEL differs from the queued accepted history context."));
    const auto pointerBytes=resultBytes(latest,64*1024);const auto pointer=resultObject(pointerBytes);resultKeys(pointer,{"schema","envelopeFile","envelopeSha256"});requireResult(pointer.value("schema")=="range.native-checkpoint-pointer/v2",tr("Compact accepted history requires the v2 committed pointer."));
    const auto envelopePath=directory.filePath(resultBasename(pointer.value("envelopeFile")));const auto envelopeBytes=resultBytes(envelopePath,64*1024);const auto envelope=resultObject(envelopeBytes);
    requireResult(QCryptographicHash::hash(envelopeBytes,QCryptographicHash::Sha256)==resultSha(pointer.value("envelopeSha256")),tr("Committed accepted history envelope SHA-256 changed."));
    auto expected=context;expected.remove("schema");expected.remove("modelSha256");expected["inputModelSha256"]=context.value("modelSha256");auto keys=expected.keys();keys<<"schema"<<"acceptedSeq"<<"timeS"<<"modelFile"<<"checkpointModelSha256"<<"modelAcceptedSeq"<<"stateFingerprintSha256"<<"traceFile"<<"traceHeaderSha256"<<"traceCursor";resultKeys(envelope,keys);
    requireResult(envelope.value("schema")=="range.native-checkpoint/v2"&&envelope.value("traceFile")=="accepted-trace.rpt",tr("Unsupported accepted history envelope."));for(auto it=expected.begin();it!=expected.end();++it)requireResult(envelope.value(it.key())==it.value(),tr("Accepted history envelope differs from its frozen execution context."));
    QFile trace(tracePath);safeResultPath(tracePath);requireResult(trace.open(QIODevice::ReadOnly),tr("Cannot open compact accepted history."));const auto header=PiezoAcceptedTrace::readHeader(trace);trace.close();
    const auto contextSha=QCryptographicHash::hash(QJsonDocument(context).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256);
    requireResult(header.headerSha32==resultSha(envelope.value("traceHeaderSha256"))&&header.contextSha256==contextSha&&header.sourceFingerprint==fingerprint&&header.dt==context.value("dtS").toDouble(-1),tr("Accepted history header differs from source or frozen context."));
    const auto cursor=envelope.value("traceCursor").toObject();resultKeys(cursor,{"byteEnd","lastRecordOffset","sequence","recordSha256","stateSha256"});
    proof->cursor.byteEnd=qint64(resultInteger(cursor.value("byteEnd"),PiezoAcceptedTrace::MaxFileBytes));proof->cursor.lastRecordOffset=qint64(resultInteger(cursor.value("lastRecordOffset"),PiezoAcceptedTrace::MaxFileBytes));proof->cursor.sequence=resultInteger(cursor.value("sequence"),header.targetSteps);proof->cursor.recordSha32=resultSha(cursor.value("recordSha256"));proof->cursor.stateSha32=resultSha(cursor.value("stateSha256"));
    requireResult(proof->cursor.sequence==resultInteger(envelope.value("acceptedSeq"),header.targetSteps)&&proof->cursor.stateSha32==resultSha(envelope.value("stateFingerprintSha256"))&&envelope.value("timeS").isDouble()&&envelope.value("timeS").toDouble()==header.initialTime+double(proof->cursor.sequence)*header.dt,tr("Accepted history cursor and original accepted clock differ."));
    resultInteger(envelope.value("modelAcceptedSeq"),proof->cursor.sequence);const auto modelPath=directory.filePath(resultBasename(envelope.value("modelFile")));safeResultPath(modelPath);rawResultModel(modelPath);requireResult(fileDigest(modelPath)==QString::fromLatin1(resultSha(envelope.value("checkpointModelSha256")).toHex()),tr("Accepted history full MODEL checkpoint changed."));
    resultBasename(header.baseModelFile);proof->baseModelPath=directory.filePath(header.baseModelFile);proof->baseModelSha256=header.baseModelSha256;proof->tracePath=tracePath;proof->expectedHeaderSha256=header.headerSha32;proof->expectedContextSha256=contextSha;proof->expectedSourceFingerprint=fingerprint;
    *files={latest,envelopePath,inputPath,modelPath,proof->baseModelPath,tracePath,paths.first()};hashes->clear();for(const auto &file:*files){safeResultPath(file);hashes->append(fileDigest(file));}return true;
}
bool PiezoWorkbenchWidget::exportAcceptedTimeSeriesTo(const QStringList &paths,const QString &path) {
    beginPerformanceRequest("csv",{{"nativePaths",QJsonArray::fromStringList(paths)},{"destination",path}});
    QJsonObject document;RModel model;if(!savedSourceSnapshot(&document,&model))return false;
    if(targetsSelectedDocument(session,path)){status->setText(tr("Choose a separate CSV file; the current model file is protected."));return false;}
    try{
        PiezoResultSeries::TraceProof proof;QStringList authorityFiles,authorityHashes;const auto boundSource=sourceFingerprint(model);
        if(discoverAcceptedTrace(paths,boundSource,&proof,&authorityFiles,&authorityHashes)){
            bindPerformanceTrace(proof,document);
            QString error;RPiezoArtifactService::Report report;
            const auto finalAuthority=[&]{QJsonObject after;RModel current;requireResult(savedSourceSnapshot(&after,&current)&&sourceFingerprint(current)==boundSource,tr("Source changed during accepted history CSV export."));for(const auto &key:{"documentId","sessionEpoch","documentRevision","project","draft"})requireResult(document.value(key)==after.value(key),tr("Source changed during accepted history CSV export."));
                PiezoResultSeries::TraceProof fresh;QStringList freshFiles,freshHashes;requireResult(discoverAcceptedTrace(paths,boundSource,&fresh,&freshFiles,&freshHashes)&&freshFiles==authorityFiles&&freshHashes==authorityHashes,tr("Committed accepted history authority changed during CSV export."));};
            if(!RPiezoArtifactService::writeAcceptedTraceTimeSeries(proof,path,&error,&report,finalAuthority)){status->setText(error);return false;}
            performanceEvent("csv_committed",{{"family",report.modelFamily==QStringLiteral("axialRod")?QStringLiteral("axial"):QStringLiteral("layered")},{"modelFamily",report.modelFamily},{"sourceFingerprint",report.sourceFingerprint},{"totalRecords",double(report.recordCount)},{"firstTimeS",report.firstTime},{"lastTimeS",report.lastTime},{"continuousFE",report.continuousFE},{"destination",path},{"baseModelSha256",QString::fromLatin1(proof.baseModelSha256.toHex())},{"headerSha256",QString::fromLatin1(proof.expectedHeaderSha256.toHex())},{"contextSha256",QString::fromLatin1(proof.expectedContextSha256.toHex())},{"terminalStateSha256",QString::fromLatin1(proof.cursor.stateSha32.toHex())}});
            status->setText(tr("Exported %1 certified compact accepted records, %2–%3 s, from the committed original FE grid. No interpolation or unprovided reaction/energy claim.").arg(report.recordCount).arg(report.firstTime,0,'g',10).arg(report.lastTime,0,'g',10));return true;
        }
        const bool axial=model.getProblemSetup().getStructuralPiezoAxialSetup().enabled;
        const auto fingerprint=[](const RModel &m,bool axial){if(axial){const auto &s=m.getProblemSetup().getStructuralPiezoAxialSetup();s.validate(m);return s.computeSourceFingerprint(m);}const auto &s=m.getProblemSetup().getStructuralPiezoLayeredSetup();s.validate(m);return s.computeSourceFingerprint(m);};
        const auto source=fingerprint(model,axial);
        for(const auto &recordPath:paths){RModel record;record.read(recordPath);
            if(record.getProblemSetup().getStructuralPiezoAxialSetup().enabled!=axial||fingerprint(record,axial)!=source){status->setText(tr("A selected accepted record belongs to another source experiment."));return false;}}
        QString error;RPiezoArtifactService::Report report;
        if(!RPiezoArtifactService::writeAcceptedTimeSeries(paths,path,&error,&report,source)){status->setText(error);return false;}
        performanceEvent("csv_committed",{{"family",report.modelFamily==QStringLiteral("axialRod")?QStringLiteral("axial"):QStringLiteral("layered")},{"modelFamily",report.modelFamily},{"sourceFingerprint",report.sourceFingerprint},{"totalRecords",double(report.recordCount)},{"firstTimeS",report.firstTime},{"lastTimeS",report.lastTime},{"continuousFE",report.continuousFE},{"destination",path}});
        status->setText(tr("Exported %1 certified accepted native samples, %2–%3 s. %4 Coverage includes selected accepted records; no interpolation or complete-history claim.")
            .arg(report.recordCount).arg(report.firstTime,0,'g',10).arg(report.lastTime,0,'g',10)
            .arg(report.continuousFE?tr("Consecutive selected FE steps."):tr("Selected FE steps contain gaps.")));return true;
    }catch(const std::exception &e){status->setText(QString::fromUtf8(e.what()));return false;}
    catch(...){status->setText(tr("Selected native records failed source qualification."));return false;}
}
bool PiezoWorkbenchWidget::exportCurrentAcceptedTimeSeriesTo(const QString &path) {
    if(!acceptedSeriesCurrent(true)||acceptedSelectionPaths.isEmpty()){
        clearAcceptedSeries(tr("Accepted results changed. Open or wait for certified results before exporting them."));return false;
    }
    if(!acceptedNativeJobId.isEmpty()){
        QJsonObject document;RModel model;if(!savedSourceSnapshot(&document,&model))return false;
        const auto job=SolverManager::getInstance().jobQuery(acceptedNativeJobId);QString reason;
        if(!certifyCompletedNativeJob(acceptedNativeJobId,job,acceptedSelectionPaths.first(),document,&reason)){
            clearAcceptedSeries(reason);status->setText(reason);return false;
        }
    }
    return exportAcceptedTimeSeriesTo(acceptedSelectionPaths,path);
}
void PiezoWorkbenchWidget::clearAcceptedSeries(const QString &reason) {
    clearAcceptedNativeWatch();
    acceptedSeries.reset();acceptedNumericSeries.reset();acceptedTraceProof={};acceptedSeriesDocument={};acceptedSeriesPaths.clear();acceptedSeriesHashes.clear();acceptedSeriesFileStamps={};acceptedSelectionPaths.clear();acceptedNativeJobId.clear();
    for(auto *choice:{resultPort,resultQuantity})if(choice){const QSignalBlocker blocker(choice);choice->clear();choice->setEnabled(false);}
    if(resultReference)resultReference->setEnabled(false);
    if(resultView)resultView->setEnabled(false);
    if(resultWindow)resultWindow->setEnabled(false);
    if(resultRemoveMean)resultRemoveMean->setEnabled(false);
    if(resultCoverage)resultCoverage->setText(reason);
    if(resultPlot){resultPlot->setDisplayHeld(false);resultPlot->setFullExportAvailable(false);resultPlot->setDataset({});}
}
void PiezoWorkbenchWidget::clearAcceptedNativeWatch() {
    ++acceptedNativeWatchGeneration;
    if(acceptedNativeWatcher){
        disconnect(acceptedNativeWatcher,nullptr,this,nullptr);acceptedNativeWatcher->deleteLater();acceptedNativeWatcher=nullptr;
    }
    acceptedNativeWatchFiles.clear();acceptedNativeWatchDirectories.clear();acceptedNativeFileStamps={};
}
bool PiezoWorkbenchWidget::captureAcceptedNativeWatch(const QJsonObject &job,QString *reason) {
    clearAcceptedNativeWatch();
    const auto reject=[&](const QString &message){if(reason)*reason=message;clearAcceptedNativeWatch();return false;};
    const quint64 watchGeneration=acceptedNativeWatchGeneration;
    acceptedNativeWatcher=new QFileSystemWatcher(this);
    const auto revokeChangedNativeResult=[this,watchGeneration](const QString &){
        if(watchGeneration!=acceptedNativeWatchGeneration||!acceptedNativeWatcher||acceptedNativeJobId.isEmpty())return;
        const auto changed=tr("A registered native result, source, or job-registry file changed after certification; the displayed result was revoked.");
        clearAcceptedSeries(changed);status->setText(changed);
    };
    connect(acceptedNativeWatcher,&QFileSystemWatcher::fileChanged,this,revokeChangedNativeResult);
    connect(acceptedNativeWatcher,&QFileSystemWatcher::directoryChanged,this,revokeChangedNativeResult);
    const auto stampInfo=[](const QFileInfo &info)->QJsonObject{
        const auto modified=info.lastModified();
        QJsonObject stamp{{"kind",info.isDir()?QStringLiteral("directory"):QStringLiteral("file")},
                          {"canonicalPath",info.canonicalFilePath()},
                          {"size",QString::number(info.size())},
                          {"modifiedMs",QString::number(modified.toMSecsSinceEpoch())}};
#ifdef Q_OS_WIN
        const auto changeTime=nativeFileChangeTime(info.canonicalFilePath());
        if(changeTime.isEmpty())return {};
        stamp.insert("changeTimeTicks",changeTime);
#endif
        return stamp;
    };
    const auto addFile=[&](const QString &path,bool watch)->bool{
        QFileInfo info(path);
        if(!info.exists()||!info.isFile()||info.isSymLink()||info.canonicalFilePath().isEmpty()||!info.lastModified().isValid())return false;
        const auto canonical=info.canonicalFilePath();const auto stamp=stampInfo(info);
        if(stamp.isEmpty())return false;
        acceptedNativeFileStamps.insert(canonical,stamp);
        if(watch&&!acceptedNativeWatchFiles.contains(canonical))acceptedNativeWatchFiles.append(canonical);
        return true;
    };
    const auto binding=job.value("resultSourceBinding").toObject();
    if(!addFile(job.value("resultArtifactRef").toString(),true)||
       !addFile(job.value("resultManifestRef").toString(),true)||
       !addFile(job.value("nativeJobRecordRef").toString(),true)||
       !addFile(binding.value("resolvedSourcePath").toString(),true))
        return reject(tr("A certified native result, manifest, source, or job record cannot be monitored."));
    const auto sourceArtifacts=binding.value("sourceArtifacts").toArray();
    if(sourceArtifacts.isEmpty()||sourceArtifacts.size()>32)
        return reject(tr("The certified native result source chain is outside its monitoring bound."));
    for(const auto &value:sourceArtifacts){
        if(!addFile(value.toObject().value("path").toString(),true))
            return reject(tr("A certified native source-chain file cannot be monitored."));
    }
    if(acceptedSeriesPaths.isEmpty()||acceptedSeriesPaths.size()>32)
        return reject(tr("The accepted-history authority file set is outside its monitoring bound."));
    for(const auto &path:acceptedSeriesPaths){
        if(!addFile(path,true))return reject(tr("A certified accepted-history authority file cannot be monitored."));
    }
    if(!acceptedTraceProof.tracePath.isEmpty()){
        QFileInfo traceDirectory(QFileInfo(acceptedTraceProof.tracePath).absolutePath());
        if(!traceDirectory.exists()||!traceDirectory.isDir()||traceDirectory.isSymLink()||
           traceDirectory.canonicalFilePath().isEmpty()||!traceDirectory.lastModified().isValid())
            return reject(tr("The compact accepted-history directory cannot be monitored."));
        const auto canonicalTraceDirectory=traceDirectory.canonicalFilePath();
        const auto stamp=stampInfo(traceDirectory);
        if(stamp.isEmpty())return reject(tr("The compact accepted-history directory cannot be monitored."));
        acceptedNativeFileStamps.insert(canonicalTraceDirectory,stamp);
        if(!acceptedNativeWatchDirectories.contains(canonicalTraceDirectory))
            acceptedNativeWatchDirectories.append(canonicalTraceDirectory);
    }
    const QString registryDirectory=QFileInfo(job.value("nativeJobRecordRef").toString()).absolutePath();
    QFileInfo registryInfo(registryDirectory);
    if(!registryInfo.exists()||!registryInfo.isDir()||registryInfo.isSymLink()||registryInfo.canonicalFilePath().isEmpty()||!registryInfo.lastModified().isValid())
        return reject(tr("The native job registry directory cannot be monitored."));
    const auto canonicalRegistryDirectory=registryInfo.canonicalFilePath();
    const auto registryStamp=stampInfo(registryInfo);
    if(registryStamp.isEmpty())return reject(tr("The native job registry directory cannot be monitored."));
    acceptedNativeFileStamps.insert(canonicalRegistryDirectory,registryStamp);
    if(!acceptedNativeWatchDirectories.contains(canonicalRegistryDirectory))
        acceptedNativeWatchDirectories.append(canonicalRegistryDirectory);
    const auto registryEntries=QDir(canonicalRegistryDirectory).entryInfoList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot,QDir::Name);
    for(const auto &entry:registryEntries){
        const auto name=entry.fileName();
        if(name==QStringLiteral("job.json")||name.startsWith(QStringLiteral("job-record-r"))||name.startsWith(QStringLiteral("commit-r"))){
            if(!addFile(entry.absoluteFilePath(),false))return reject(tr("A native job registry chain file cannot be monitored."));
        }
    }
    const auto failedFiles=acceptedNativeWatcher->addPaths(acceptedNativeWatchFiles);
    const auto failedDirectories=acceptedNativeWatcher->addPaths(acceptedNativeWatchDirectories);
    if(!failedFiles.isEmpty()||!failedDirectories.isEmpty())
        return reject(tr("The file monitor could not watch every certified native result source."));
    if(reason)reason->clear();
    return true;
}
bool PiezoWorkbenchWidget::acceptedNativeWatchCurrent() const {
    if(acceptedNativeJobId.isEmpty())return true;
    if(acceptedNativeFileStamps.isEmpty()||!acceptedNativeWatcher)return false;
    const auto watchedFiles=acceptedNativeWatcher->files(),watchedDirectories=acceptedNativeWatcher->directories();
    if(watchedFiles.size()!=acceptedNativeWatchFiles.size()||watchedDirectories.size()!=acceptedNativeWatchDirectories.size())return false;
    for(const auto &path:acceptedNativeWatchFiles)if(!watchedFiles.contains(path))return false;
    for(const auto &path:acceptedNativeWatchDirectories)if(!watchedDirectories.contains(path))return false;
    for(auto it=acceptedNativeFileStamps.constBegin();it!=acceptedNativeFileStamps.constEnd();++it){
        const QFileInfo info(it.key());const auto modified=info.lastModified();
        if(!info.exists()||info.isSymLink()||info.canonicalFilePath().isEmpty()||!modified.isValid())return false;
        QJsonObject stamp{{"kind",info.isDir()?QStringLiteral("directory"):QStringLiteral("file")},
                          {"canonicalPath",info.canonicalFilePath()},
                          {"size",QString::number(info.size())},
                          {"modifiedMs",QString::number(modified.toMSecsSinceEpoch())}};
#ifdef Q_OS_WIN
        const auto changeTime=nativeFileChangeTime(info.canonicalFilePath());
        if(changeTime.isEmpty())return false;
        stamp.insert("changeTimeTicks",changeTime);
#endif
        if(stamp!=it.value().toObject())return false;
    }
    return true;
}
bool PiezoWorkbenchWidget::acceptedSeriesCurrent(bool verifyRecordFiles,bool externalSourcesKnownCurrent) const {
    if((!acceptedSeries&&!acceptedNumericSeries)||recipe!=savedRecipe||
       (!externalSourcesKnownCurrent&&!externalSourcesMetadataCurrent()))return false;
    const auto document=currentDocument();if(document.isEmpty())return false;
    for(const auto &key:{"documentId","sessionEpoch","documentRevision","project","draft"})if(document.value(key)!=acceptedSeriesDocument.value(key))return false;
    if(!acceptedNativeWatchCurrent())return false;
    if(acceptedSeriesPaths.size()!=acceptedSeriesFileStamps.size())return false;
    for(int i=0;i<acceptedSeriesPaths.size();++i){
        const auto stamp=acceptedFileStamp(acceptedSeriesPaths[i]);
        if(stamp.isEmpty()||stamp!=acceptedSeriesFileStamps[i].toObject())return false;
    }
    if(acceptedNumericSeries){const auto pending=QDir(QFileInfo(acceptedTraceProof.tracePath).absolutePath()).filePath("pending-publication.json");if(QFileInfo::exists(pending)||QFileInfo(pending).isSymLink())return false;}
    if(verifyRecordFiles)try{if(acceptedSeriesPaths.size()!=acceptedSeriesHashes.size())return false;for(int i=0;i<acceptedSeriesPaths.size();++i)if(fileDigest(acceptedSeriesPaths[i])!=acceptedSeriesHashes[i])return false;}catch(...){return false;}
    return true;
}
bool PiezoWorkbenchWidget::certifyCompletedNativeJob(const QString &jobId,const QJsonObject &job,
                                                      const QString &artifactPath,const QJsonObject &document,
                                                      QString *reason) const {
    const auto reject=[&](const QString &message){if(reason)*reason=message;return false;};
    if(jobId.isEmpty()||job.value("found")!=QJsonValue(true)||job.value("jobId").toString()!=jobId)
        return reject(tr("The completed native job is unavailable or its registry is conflicted."));
    if(job.value("state").toString()!="succeeded")
        return reject(tr("Native job ended in '%1'; only a completed successful result can be bound.").arg(job.value("state").toString()));
    if(job.value("eligibility").toString()=="conflict")
        return reject(tr("The completed native job registry or result artifacts are conflicted."));
    if(!job.value("provenanceFrozen").toBool()||!job.value("resultArtifactVerified").toBool()||
       !job.value("resultSourceBindingVerified").toBool()||!job.value("resultManifestVerified").toBool())
        return reject(tr("The completed native job has no fully verified registered result."));
    const auto stamp=job.value("sourceRuntimeStamp").toObject();
    const double revision=document.value("documentRevision").toDouble(-1.0);
    const QFileInfo attachedArtifact(artifactPath);
    const auto applied=appliedNativeResults.value(attachedArtifact.canonicalFilePath());
    const QString currentRevision=std::isfinite(revision)&&revision>=0.0&&std::floor(revision)==revision
        ?QString::number(static_cast<quint64>(revision)):QString();
    const bool exactSourceRevision=stamp.value("revision").toString()==currentRevision;
    const bool exactAttachedRevision=!applied.isEmpty()&&
        applied.value("documentId")==stamp.value("documentId")&&
        applied.value("sessionEpoch")==stamp.value("sessionEpoch")&&
        applied.value("sourceRevision")==stamp.value("revision")&&
        applied.value("attachedRevision")==currentRevision;
    if(document.value("documentId").toString().isEmpty()||document.value("sessionEpoch").toString().isEmpty()||
       currentRevision.isEmpty()||stamp.value("documentId")!=document.value("documentId")||
       stamp.value("sessionEpoch")!=document.value("sessionEpoch")||
       (!exactSourceRevision&&!exactAttachedRevision))
        return reject(tr("The completed native job belongs to a different or stale source document."));
    const QFileInfo artifact(artifactPath),registered(job.value("resultArtifactRef").toString());
    const QFileInfo manifest(job.value("resultManifestRef").toString());
    const auto sha256=[](const QString &value){
        if(value.size()!=64)return false;
        for(const QChar c:value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
        return true;
    };
    if(artifactPath.isEmpty()||!artifact.isFile()||artifact.isSymLink()||artifact.canonicalFilePath().isEmpty()||
       !registered.isFile()||registered.canonicalFilePath().isEmpty()||!sameResultPath(artifactPath,registered.absoluteFilePath())||
       !manifest.isFile()||manifest.isSymLink()||!sha256(job.value("resultArtifactSha256").toString())||
       !sha256(job.value("resultManifestSha256").toString()))
        return reject(tr("The completed native job's artifact path or certificate does not match its registry."));
    if(reason)reason->clear();
    return true;
}
void PiezoWorkbenchWidget::addAcceptedResultEditor(QVBoxLayout *layout) {
    auto *group=new QGroupBox(tr("Certified named electrode time series and spectra"),this);auto *v=new QVBoxLayout(group);
    auto *load=new QPushButton(tr("Open native result / accepted records…"),group);load->setObjectName("piezoShowAcceptedSeries");
    const QString fullOpenLabel=tr("Open native result / accepted records\nand show the selected view…");load->setAccessibleName(fullOpenLabel);load->setToolTip(fullOpenLabel);
    connect(load,&QPushButton::clicked,this,&PiezoWorkbenchWidget::showAcceptedTimeSeries);v->addWidget(load);

    auto *fields=new QWidget(group);fields->setObjectName("piezoResultFields");fields->setMinimumWidth(0);fields->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
    auto *fieldGrid=new QGridLayout(fields);fieldGrid->setObjectName("piezoResultFieldGrid");fieldGrid->setContentsMargins(0,0,0,0);
    auto addResultField=[&](QComboBox *choice,const QString &labelText,const QString &fieldName){
        auto *field=new QWidget(fields);field->setObjectName(fieldName);field->setMinimumWidth(0);field->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        auto *form=new QFormLayout(field);form->setContentsMargins(0,0,0,0);form->setRowWrapPolicy(QFormLayout::WrapLongRows);form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        auto *label=new QLabel(labelText,field);label->setBuddy(choice);label->setWordWrap(true);choice->setAccessibleName(labelText);choice->setMinimumContentsLength(12);
        choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);choice->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        choice->setToolTip(choice->currentText());form->addRow(label,choice);return std::pair<QWidget*,QFormLayout*>{field,form};
    };
    resultPort=new QComboBox(group);resultPort->setObjectName("piezoResultPort");
    resultQuantity=new QComboBox(group);resultQuantity->setObjectName("piezoResultQuantity");
    resultReference=new QComboBox(group);resultReference->setObjectName("piezoResultReference");resultReference->addItem(tr("Absolute value"),"absolute");resultReference->addItem(tr("Increment from working point"),"delta");
    resultView=new QComboBox(group);resultView->setObjectName("piezoResultView");resultView->addItem(tr("Time"),"time");resultView->addItem(tr("Single-sided amplitude spectrum"),"amplitude");resultView->addItem(tr("Power spectral density"),"psd");resultView->setCurrentIndex(resultView->findData(resultViewKind));
    const auto portField=addResultField(resultPort,tr("Named port / readout mode"),"piezoResultPortField");
    const auto quantityField=addResultField(resultQuantity,tr("Physical quantity / unit"),"piezoResultQuantityField");
    const auto referenceField=addResultField(resultReference,tr("Reference"),"piezoResultReferenceField");
    const auto viewField=addResultField(resultView,tr("View"),"piezoResultViewField");
    QWidget *fieldWidgets[]{portField.first,quantityField.first,referenceField.first,viewField.first};
    QFormLayout *fieldForms[]{portField.second,quantityField.second,referenceField.second,viewField.second};
    const int wideLeft=std::max(resultFieldInlineWidth(fieldForms[0],resultPort),resultFieldInlineWidth(fieldForms[2],resultReference));
    const int wideRight=std::max(resultFieldInlineWidth(fieldForms[1],resultQuantity),resultFieldInlineWidth(fieldForms[3],resultView));
    int gridLeft=0,gridTop=0,gridRight=0,gridBottom=0;fieldGrid->getContentsMargins(&gridLeft,&gridTop,&gridRight,&gridBottom);
    const int gridSpacing=fieldGrid->horizontalSpacing()>=0?fieldGrid->horizontalSpacing():fieldGrid->spacing();
    const int wideThreshold=wideLeft+wideRight+std::max(0,gridSpacing)+gridLeft+gridRight;
    fieldGrid->setColumnStretch(0,std::max(1,wideLeft));fieldGrid->setColumnStretch(1,std::max(1,wideRight));
    fields->setProperty("piezoResultWideThreshold",wideThreshold);v->addWidget(fields);

    auto *spectralForm=new QFormLayout;spectralForm->setObjectName("piezoResultSpectralForm");spectralForm->setRowWrapPolicy(QFormLayout::WrapLongRows);spectralForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    resultWindow=new QComboBox(group);resultWindow->setObjectName("piezoResultWindow");resultWindow->addItem(tr("Periodic Hann"),"hann");resultWindow->addItem(tr("Rectangular"),"rectangular");resultWindow->setCurrentIndex(resultWindow->findData(resultWindowKind));
    auto *windowLabel=new QLabel(tr("Spectrum window"),group);windowLabel->setBuddy(resultWindow);windowLabel->setWordWrap(true);resultWindow->setAccessibleName(windowLabel->text());resultWindow->setMinimumContentsLength(12);resultWindow->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);resultWindow->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);resultWindow->setToolTip(resultWindow->currentText());
    spectralForm->addRow(windowLabel,resultWindow);
    resultRemoveMean=new QCheckBox(tr("Remove mean before spectrum"),group);resultRemoveMean->setObjectName("piezoResultRemoveMean");resultRemoveMean->setChecked(resultRemoveMeanValue);resultRemoveMean->setAccessibleName(resultRemoveMean->text());spectralForm->addRow(resultRemoveMean);
    updateSpectralResultRows(spectralForm,resultView,resultWindow,resultRemoveMean);v->addLayout(spectralForm);

    resultPlot=new SeriesPlotWidget(group);resultPlot->setObjectName("piezoAcceptedPlot");resultPlot->setMinimumHeight(280);connect(resultPlot,&SeriesPlotWidget::datasetPainted,this,&PiezoWorkbenchWidget::acceptedDatasetPainted);v->addWidget(resultPlot);
    resultCoverage=note(tr("Select a native result to open its complete committed compact FE history automatically, or select legacy accepted RAM/RBM records from the current saved experiment."),group);resultCoverage->setObjectName("piezoResultCoverage");v->addWidget(resultCoverage);
    resultPort->setEnabled(false);resultQuantity->setEnabled(false);resultReference->setEnabled(false);resultView->setEnabled(false);resultWindow->setEnabled(false);resultRemoveMean->setEnabled(false);
    auto *modeFilter=new ResponsiveResultFieldGrid(fields,fieldGrid,fieldWidgets[0],fieldWidgets[1],fieldWidgets[2],fieldWidgets[3],wideThreshold);modeFilter->setObjectName("piezoResultFieldGridModeFilter");
    connect(resultPort,&QComboBox::currentIndexChanged,this,[this]{populateResultQuantities();});
    connect(resultQuantity,&QComboBox::currentIndexChanged,this,[this]{renderAcceptedSeries(true);});connect(resultReference,&QComboBox::currentIndexChanged,this,[this]{renderAcceptedSeries(true);});
    connect(resultView,&QComboBox::currentIndexChanged,this,[this,spectralForm]{resultViewKind=resultView->currentData().toString();updateSpectralResultRows(spectralForm,resultView,resultWindow,resultRemoveMean);renderAcceptedSeries(true);});
    connect(resultWindow,&QComboBox::currentIndexChanged,this,[this]{resultWindowKind=resultWindow->currentData().toString();renderAcceptedSeries(true);});
    connect(resultRemoveMean,&QCheckBox::toggled,this,[this](bool value){resultRemoveMeanValue=value;renderAcceptedSeries(true);});
    layout->insertWidget(std::max(0,layout->count()-1),group);
    QWidget::setTabOrder(load,resultPort);QWidget::setTabOrder(resultPort,resultQuantity);QWidget::setTabOrder(resultQuantity,resultReference);QWidget::setTabOrder(resultReference,resultView);
    QWidget::setTabOrder(resultView,resultWindow);QWidget::setTabOrder(resultWindow,resultRemoveMean);
    if(auto *hold=resultPlot->findChild<QCheckBox*>("seriesPlotHold"))QWidget::setTabOrder(resultRemoveMean,hold);
    if(auto *hold=resultPlot->findChild<QCheckBox*>("seriesPlotHold"))if(auto *fit=resultPlot->findChild<QPushButton*>("seriesPlotFit"))QWidget::setTabOrder(hold,fit);
    if(auto *fit=resultPlot->findChild<QPushButton*>("seriesPlotFit"))if(auto *exportButton=resultPlot->findChild<QPushButton*>("seriesPlotExport"))QWidget::setTabOrder(fit,exportButton);
    if(acceptedSeries||acceptedNumericSeries){
        const QSignalBlocker blocker(resultPort);for(const auto &p:acceptedReadouts())resultPort->addItem(tr("%1 | %2 | %3").arg(p.portId,readoutModeName(p.mode),p.unit),p.portId);
        resultPort->setToolTip(resultPort->currentText());
        resultPort->setEnabled(resultPort->count()>0);resultReference->setEnabled(resultPort->count()>0);populateResultQuantities();
    }
}
QVector<PiezoResultSeries::Readout> PiezoWorkbenchWidget::acceptedReadouts() const {
    if(acceptedNumericSeries)return acceptedNumericSeries->readouts;
    if(acceptedSeries&&!acceptedSeries->frames.isEmpty())return acceptedSeries->frames.first()->readouts;
    return {};
}
void PiezoWorkbenchWidget::populateResultQuantities() {
    if(!resultQuantity)return;
    {const QSignalBlocker blocker(resultQuantity);resultQuantity->clear();
        if(acceptedSeries||acceptedNumericSeries)for(const auto &p:acceptedReadouts())if(p.portId==resultPort->currentData().toString()){
            resultQuantity->addItem(tr("Readout (%1)").arg(p.unit),"piezoReadout");resultQuantity->addItem(tr("Electrode voltage (V)"),"electrodeVoltage");resultQuantity->addItem(tr("Positive-electrode charge (C)"),"positiveElectrodeCharge");
            if(p.amplifierAvailable)resultQuantity->addItem(tr("Ideal amplifier voltage (V)"),"idealAmplifierVoltage");break;
        }resultQuantity->setEnabled(resultQuantity->count()>0);
    }resultQuantity->setToolTip(resultQuantity->currentText());renderAcceptedSeries();
}
void PiezoWorkbenchWidget::renderAcceptedSeries(bool verifyRecordFiles) {
    const bool spectral=resultViewKind!="time";
    if(resultPlot)updateSpectralResultRows(resultPlot->parentWidget()->findChild<QFormLayout*>("piezoResultSpectralForm"),resultView,resultWindow,resultRemoveMean);
    if((!acceptedSeries&&!acceptedNumericSeries)||!resultPlot||!resultPort||!resultQuantity||!resultReference)return;
    if(!acceptedSeriesCurrent(verifyRecordFiles)){clearAcceptedSeries(tr("Source experiment or accepted records changed. Select and certify records again."));return;}
    const auto port=resultPort->currentData().toString(),quantity=resultQuantity->currentData().toString(),reference=resultReference->currentData().toString();
    if(port.isEmpty()||quantity.isEmpty())return;
    if(resultView)resultView->setEnabled(true);if(resultWindow)resultWindow->setEnabled(spectral);if(resultRemoveMean)resultRemoveMean->setEnabled(spectral);
    SeriesPlotWidget::Dataset plot;plot.viewKind=SeriesPlotWidget::ViewKind::Time;plot.title=tr("Accepted named electrode samples");plot.xLabel=tr("Native accepted time");plot.xUnit="s";
    SeriesPlotWidget::Trace trace;trace.sourceId=acceptedNumericSeries?acceptedNumericSeries->sourceFingerprint:acceptedSeries->sourceFingerprint;trace.artifactSha256=acceptedNumericSeries?acceptedNumericSeries->artifactSha256:acceptedSeries->artifactSha256;
    const qsizetype count=acceptedNumericSeries?acceptedNumericSeries->times.size():acceptedSeries->frames.size();qsizetype displayed=std::min<qsizetype>(count,1000);bool actionRecord=false;QString spectralCaption;
    qsizetype numericColumn=-1;
    if(acceptedNumericSeries){const auto &channels=acceptedNumericSeries->channels;
        for(qsizetype i=0;i<channels.size();++i)if(channels[i].portId==port&&channels[i].quantity==quantity&&channels[i].axis==reference){
            if(numericColumn>=0){clearAcceptedSeries(tr("Selected compact port channel is duplicated."));return;}numericColumn=i;trace.id=channels[i].channelId;trace.unit=channels[i].unit;actionRecord=channels[i].actionRecord;
        }
        if(numericColumn<0||numericColumn>=acceptedNumericSeries->columns.size()||acceptedNumericSeries->columns[numericColumn].size()!=count){clearAcceptedSeries(tr("Selected compact port quantity is unavailable."));return;}
    }else for(qsizetype i=0;i<count;++i){const auto &frame=*acceptedSeries->frames[i];
        const auto signal=std::find_if(frame.channels.begin(),frame.channels.end(),[&](const auto &s){return s.portId==port&&s.quantity==quantity&&s.axis==reference;});
        if(signal==frame.channels.end()||(!trace.unit.isEmpty()&&trace.unit!=signal->unit)){
            clearAcceptedSeries(tr("Selected certified records do not provide one consistent named-port quantity and unit."));return;
        }
        if(i==0){trace.id=signal->channelId;trace.unit=signal->unit;actionRecord=signal->actionRecord;}
    }
    if(spectral){
        PiezoSpectrum::Options options;options.window=resultWindowKind=="hann"?PiezoSpectrum::Window::PeriodicHann:PiezoSpectrum::Window::Rectangular;options.removeMean=resultRemoveMeanValue;
        const auto computed=acceptedNumericSeries?PiezoSpectrum::calculate(*acceptedNumericSeries,trace.id,options):PiezoSpectrum::calculate(*acceptedSeries,trace.id,options);
        if(!computed.valid){resultPlot->setDisplayHeld(false);resultPlot->setDataset({});resultCoverage->setText(computed.message);status->setText(computed.message);return;}
        if(!acceptedSeriesCurrent(verifyRecordFiles)){clearAcceptedSeries(tr("Source experiment or accepted records changed during spectrum calculation."));return;}
        const auto &s=computed.spectrum;plot.viewKind=SeriesPlotWidget::ViewKind::Spectrum;plot.title=tr("Accepted named electrode spectrum");plot.xLabel=tr("Frequency");plot.xUnit="Hz";
        const auto &ordinates=resultViewKind=="psd"?s.psd:s.amplitude;trace.unit=resultViewKind=="psd"?s.psdUnit:s.amplitudeUnit;
        // All original records entered the DFT. Only frequency-bin display is
        // reduced, preserving first/last and bucket extrema with true bin IDs.
        QVector<qsizetype> bins;
        if(ordinates.size()<=1000){for(qsizetype k=0;k<ordinates.size();++k)bins.append(k);}
        else{std::set<qsizetype> selected{0,ordinates.size()-1};const qsizetype buckets=249,interior=ordinates.size()-2;
            for(qsizetype bucket=0;bucket<buckets;++bucket){const auto start=1+bucket*interior/buckets,end=1+(bucket+1)*interior/buckets;if(start>=end)continue;
                qsizetype minimum=start,maximum=start;for(qsizetype k=start+1;k<end;++k){if(ordinates[k]<ordinates[minimum])minimum=k;if(ordinates[k]>ordinates[maximum])maximum=k;}
                selected.insert(start);selected.insert(end-1);selected.insert(minimum);selected.insert(maximum);
            }for(const auto k:selected)bins.append(k);
        }
        for(const auto k:bins){trace.points.append(QPointF(s.frequenciesHz[k],ordinates[k]));trace.originalOrdinals.append(quint64(k));}displayed=bins.size();
        spectralCaption=tr("%1 from all %2 original samples, %3–%4 s; fs=%5 Hz, Δf=%6 Hz, Nyquist=%7 Hz. %8 window; %9. %10 of %11 bins displayed; hover ordinal is the FFT bin. DC and even-length Nyquist are not doubled.")
            .arg(resultViewKind=="psd"?tr("PSD"):tr("Single-sided amplitude")).arg(s.sampleCount).arg(s.firstTimeS,0,'g',10).arg(s.lastTimeS,0,'g',10)
            .arg(s.samplingRateHz,0,'g',10).arg(s.binWidthHz,0,'g',10).arg(s.nyquistHz,0,'g',10).arg(s.window)
            .arg(s.removeMean?tr("mean removed"):tr("DC retained")).arg(displayed).arg(s.frequenciesHz.size());
    }else if(acceptedNumericSeries){for(qsizetype i=0;i<displayed;++i){const qsizetype ordinal=displayed==1?0:i*(count-1)/(displayed-1);trace.points.append(QPointF(acceptedNumericSeries->times[ordinal],acceptedNumericSeries->columns[numericColumn][ordinal]));trace.originalOrdinals.append(quint64(ordinal));}}
    else for(qsizetype i=0;i<displayed;++i){const qsizetype ordinal=displayed==1?0:i*(count-1)/(displayed-1);const auto &frame=*acceptedSeries->frames[ordinal];
        const auto signal=std::find_if(frame.channels.begin(),frame.channels.end(),[&](const auto &s){return s.channelId==trace.id;});
        trace.points.append(QPointF(frame.timeS,signal->value));trace.originalOrdinals.append(quint64(ordinal));
    }
    trace.label=tr("%1 | %2 | %3%4").arg(port,resultQuantity->currentText(),resultReference->currentText(),actionRecord?tr(" | action record"):QString());plot.yLabel=trace.label;plot.yUnit=trace.unit;plot.traces.append(trace);
    QString error;resultPlot->setDisplayHeld(false);resultPlot->setFullExportAvailable(false);
    if(!resultPlot->setDataset(plot,&error)){clearAcceptedSeries(error);status->setText(error);return;}
    bindPerformancePlot();
    QStringList steps;const auto stepAt=[&](qsizetype i){return acceptedNumericSeries?acceptedNumericSeries->completedFE[i]:acceptedSeries->frames[i]->completedFE;};
    if(count<=12)for(qsizetype i=0;i<count;++i)steps.append(QString::number(stepAt(i)));else steps<<tr("%1–%2").arg(stepAt(0)).arg(stepAt(count-1));
    resultCoverage->setText(tr("%1 certified selected records; %2 exact %7 displayed. FE steps: %3. %4\nSampled coverage only: no interpolation or complete-history claim. %5\n%6")
        .arg(count).arg(displayed).arg(steps.join(", ")).arg((acceptedNumericSeries?acceptedNumericSeries->continuousBetweenSelectedRecords:acceptedSeries->continuousBetweenSelectedRecords)?tr("Selected FE steps are consecutive."):tr("Selected FE steps contain gaps."))
        .arg(actionRecord?tr("This quantity is an action record."):tr("This quantity is a certified electrode signal.")).arg(acceptedNumericSeries?acceptedNumericSeries->coverage:acceptedSeries->coverage).arg(spectral?tr("frequency bins"):tr("samples"))+(spectral?"\n"+spectralCaption:QString()));
    const QPointer<SeriesPlotWidget> presentedPlot(resultPlot);
    const QPointer<QWidget> canvas(resultPlot->findChild<QWidget*>("seriesPlotCanvas"));
    const quint64 sourceGeneration=contextGeneration,datasetGeneration=resultPlot->datasetGeneration();
    const auto stillPresented=[this,presentedPlot,canvas,sourceGeneration,datasetGeneration]{
        return presentedPlot&&canvas&&presentedPlot==resultPlot&&sourceGeneration==contextGeneration&&
            presentedPlot->datasetGeneration()==datasetGeneration&&presentedPlot->hasDataset()&&
            canvas->isVisible()&&acceptedSeriesCurrent();
    };
    // Layout requests from the coverage text and tab contents must settle before
    // scrolling. This reveals the existing dataset; it neither reloads it nor
    // restarts its measured request/paint generation.
    QTimer::singleShot(0,this,[this,canvas,stillPresented]{
        if(!stillPresented())return;
        QList<QPointer<QScrollArea>> ancestors;
        for(QWidget *parent=canvas->parentWidget();parent;parent=parent->parentWidget())
            if(auto *scroll=qobject_cast<QScrollArea*>(parent))ancestors.append(scroll);
        if(ancestors.size()>1){
            // Only the inner tab gets room for the real canvas. The outer
            // workflow scroll area can grow its content while its top-level
            // window/dock minimum remains unchanged.
            auto *inner=ancestors.first().data();
            const int chrome=std::max(0,tabs->height()-inner->viewport()->height());
            // Allocated height can be large in a tall dock. Reserve only the
            // stable canvas minimum so a later compact dock can shrink again.
            tabs->setMinimumHeight(canvas->minimumHeight()+chrome);
            for(QWidget *parent=canvas->parentWidget();parent;parent=parent->parentWidget())
                if(parent->layout())parent->layout()->activate();
        }
        QTimer::singleShot(0,this,[canvas,ancestors,stillPresented]{
            if(!stillPresented())return;
            // Moving an inner viewport first leaves its canvas rectangle fixed
            // relative to that viewport when the outer workflow then scrolls.
            for(const auto &scroll:ancestors)if(scroll)scroll->ensureWidgetVisible(canvas,0,0);
        });
    });
}
void PiezoWorkbenchWidget::showAcceptedTimeSeries() {
    QJsonObject document;RModel model;if(!savedSourceSnapshot(&document,&model))return;
    const auto paths=QFileDialog::getOpenFileNames(this,tr("Select certified accepted records for named-port time series"),exportDirectory(),tr("Accepted native records (*.rbm *.ram)"));
    if(!paths.isEmpty()){submittedRunJobId.clear();showAcceptedTimeSeriesFrom(paths);}
}
bool PiezoWorkbenchWidget::showAcceptedTimeSeriesFrom(const QStringList &paths,bool *sourceBusy) {
    if(sourceBusy)*sourceBusy=false;
    beginPerformanceRequest("plot",{{"nativePaths",QJsonArray::fromStringList(paths)}});
    clearAcceptedSeries(tr("Certifying selected accepted native records…"));
    auto selectedBytes=[](const QStringList &files){qint64 total=0;for(const auto &file:files)total+=std::max<qint64>(0,QFileInfo(file).size());return total;};
    auto emitProfileStage=[this,&selectedBytes](const QString &stage,QElapsedTimer &timer,qint64 items,const QStringList &files){
        if(!performanceEnabled||performanceInvalid)return;
        const auto duration=timer.nsecsElapsed();
        const auto bytes=selectedBytes(files);
        const auto savedBinding=performanceBinding;performanceBinding={};
        performanceEvent("hotpath_stage",{{"stage",stage},{"durationNanoseconds",QString::number(duration)},
            {"itemCount",QString::number(items)},{"byteCount",QString::number(bytes)}});
        performanceBinding=savedBinding;timer.restart();
    };
    QElapsedTimer stageTimer;if(performanceEnabled)stageTimer.start();
    QJsonObject before;RModel model;if(!savedSourceSnapshot(&before,&model,sourceBusy))return false;
    if(performanceEnabled)stageTimer.restart();
    try{
        const auto fingerprint=sourceFingerprint(model);QStringList files=paths,hashes;PiezoResultSeries::TraceProof proof;
        std::shared_ptr<const PiezoResultSeries::Dataset> legacy;std::shared_ptr<const PiezoResultSeries::NumericDataset> numeric;
        if(discoverAcceptedTrace(paths,fingerprint,&proof,&files,&hashes)){
            emitProfileStage("authority_discovery_and_hashes",stageTimer,files.size(),files);
            const auto read=PiezoResultSeries::readAcceptedTrace(proof,namedResultChannels(savedRecipe));
            if(!read.valid||!read.dataset||read.dataset->times.isEmpty()){status->setText(read.message);if(resultCoverage)resultCoverage->setText(read.message);return false;}numeric=read.dataset;
        }else{
            for(const auto &file:paths)hashes.append(fileDigest(file));
            emitProfileStage("authority_discovery_and_hashes",stageTimer,paths.size(),paths);
            const auto read=PiezoResultSeries::readAcceptedRecords(paths,fingerprint,hashes);
            if(!read.valid||!read.dataset||read.dataset->frames.isEmpty()){status->setText(read.message);if(resultCoverage)resultCoverage->setText(read.message);return false;}legacy=read.dataset;
        }
        if(performanceEnabled)stageTimer.restart();
        QJsonArray stamps;for(int i=0;i<files.size();++i){
            requireResult(fileDigest(files[i])==hashes[i],tr("Accepted history authority changed during certification."));
            const auto stamp=acceptedFileStamp(files[i]);requireResult(!stamp.isEmpty(),tr("Accepted history authority file cannot be monitored."));stamps.append(stamp);
        }
        QJsonObject after;RModel current;if(!savedSourceSnapshot(&after,&current,sourceBusy)||sourceFingerprint(current)!=fingerprint){clearAcceptedSeries(tr("Source changed during accepted-record certification."));return false;}
        for(const auto &key:{"documentId","sessionEpoch","documentRevision","project","draft"})if(before.value(key)!=after.value(key)){clearAcceptedSeries(tr("Source changed during accepted-record certification."));return false;}
        emitProfileStage("final_authority_and_source_check",stageTimer,files.size(),files);
        acceptedSeries=std::move(legacy);acceptedNumericSeries=std::move(numeric);acceptedTraceProof=proof;acceptedSeriesDocument=after;acceptedSeriesPaths=files;acceptedSeriesHashes=hashes;acceptedSeriesFileStamps=stamps;acceptedSelectionPaths=paths;
        performanceEvent("records_certified",acceptedPerformanceIdentity());
        if(resultPort){const QSignalBlocker blocker(resultPort);resultPort->clear();for(const auto &p:acceptedReadouts())resultPort->addItem(tr("%1 | %2 | %3").arg(p.portId,readoutModeName(p.mode),p.unit),p.portId);resultPort->setToolTip(resultPort->currentText());
            resultPort->setEnabled(resultPort->count()>0);resultReference->setEnabled(resultPort->count()>0);populateResultQuantities();
        }
        emitProfileStage("ui_populate",stageTimer,acceptedNumericSeries?acceptedNumericSeries->times.size():(acceptedSeries?acceptedSeries->frames.size():0),files);
        if((!acceptedSeries&&!acceptedNumericSeries)||(resultPlot&&resultPlot->dataset().traces.isEmpty()))return false;status->setText(tr("Certified selected accepted records displayed with actual port names, units and source provenance."));return true;
    }catch(const std::exception &e){clearAcceptedSeries(QString::fromUtf8(e.what()));status->setText(QString::fromUtf8(e.what()));return false;}
    catch(...){clearAcceptedSeries(tr("Accepted records failed native source qualification."));status->setText(tr("Accepted records failed native source qualification."));return false;}
}
bool PiezoWorkbenchWidget::bindCompletedNativeJob(const QString &jobId,bool *retryable) {
    if(retryable)*retryable=false;
    if(jobId.isEmpty()||jobId!=submittedRunJobId){
        status->setText(tr("This native job was not submitted by the current Piezo Run+Confirm action; accepted results were left unchanged."));return false;
    }
    bool sourceBusy=false;QJsonObject document;RModel model;
    if(!savedSourceSnapshot(&document,&model,&sourceBusy)){if(retryable)*retryable=sourceBusy;return false;}
    const auto job=SolverManager::getInstance().jobQuery(jobId);const QString artifactPath=job.value("resultArtifactRef").toString();QString reason;
    if(!certifyCompletedNativeJob(jobId,job,artifactPath,document,&reason)){status->setText(reason);return false;}
    if(acceptedNativeJobId==jobId&&acceptedSelectionPaths==QStringList{artifactPath}&&acceptedSeriesCurrent())return true;
    if(!showAcceptedTimeSeriesFrom({artifactPath},&sourceBusy)){if(retryable)*retryable=sourceBusy;return false;}
    QJsonObject after;RModel current;
    if(!savedSourceSnapshot(&after,&current,&sourceBusy)||!acceptedSeriesCurrent()||acceptedSelectionPaths!=QStringList{artifactPath}){
        if(retryable)*retryable=sourceBusy;
        if(reason.isEmpty())reason=tr("The source or registered result changed during automatic result binding.");
        clearAcceptedSeries(reason);status->setText(reason);return false;
    }
    if(!captureAcceptedNativeWatch(job,&reason)){
        clearAcceptedSeries(reason);status->setText(reason);return false;
    }
    acceptedNativeJobId=jobId;
    QJsonObject finalDocument;RModel finalModel;const auto finalJob=SolverManager::getInstance().jobQuery(jobId);
    if(!savedSourceSnapshot(&finalDocument,&finalModel,&sourceBusy)||!certifyCompletedNativeJob(jobId,finalJob,artifactPath,finalDocument,&reason)||
       !acceptedNativeWatchCurrent()||!acceptedSeriesCurrent()||acceptedSelectionPaths!=QStringList{artifactPath}){
        if(retryable)*retryable=sourceBusy;
        if(reason.isEmpty())reason=tr("The registered result changed while its display monitor was being installed.");
        clearAcceptedSeries(reason);status->setText(reason);return false;
    }
    status->setText(tr("Automatically bound certified accepted results from native job %1.").arg(jobId.left(8)));
    return true;
}
bool PiezoWorkbenchWidget::showAcceptedTimeSeriesFromTrace(const PiezoResultSeries::TraceProof &proof) {
    beginPerformanceRequest("plot",{{"baseModelPath",proof.baseModelPath},{"tracePath",proof.tracePath},{"origin","trusted-fixture-authority"}});
    clearAcceptedSeries(tr("Certifying authority-bound accepted history…"));QJsonObject before;RModel model;if(!savedSourceSnapshot(&before,&model))return false;
    try{const auto fingerprint=sourceFingerprint(model);requireResult(fingerprint==proof.expectedSourceFingerprint,tr("Accepted history belongs to another saved experiment."));
        const auto pending=QDir(QFileInfo(proof.tracePath).absolutePath()).filePath("pending-publication.json");requireResult(!QFileInfo::exists(pending)&&!QFileInfo(pending).isSymLink(),tr("Accepted history has unresolved PENDING authority."));
        const QStringList files{proof.baseModelPath,proof.tracePath};QStringList hashes;for(const auto &file:files)hashes.append(fileDigest(file));const auto read=PiezoResultSeries::readAcceptedTrace(proof,namedResultChannels(savedRecipe));requireResult(read.valid&&read.dataset&&!read.dataset->times.isEmpty(),read.message);
        QJsonObject after;RModel current;requireResult(savedSourceSnapshot(&after,&current)&&sourceFingerprint(current)==fingerprint,tr("Source changed during accepted history certification."));for(const auto &key:{"documentId","sessionEpoch","documentRevision","project","draft"})requireResult(before.value(key)==after.value(key),tr("Source changed during accepted history certification."));QJsonArray stamps;for(int i=0;i<files.size();++i){requireResult(fileDigest(files[i])==hashes[i],tr("Accepted history changed during certification."));const auto stamp=acceptedFileStamp(files[i]);requireResult(!stamp.isEmpty(),tr("Accepted history authority file cannot be monitored."));stamps.append(stamp);}
        acceptedNumericSeries=read.dataset;acceptedTraceProof=proof;acceptedSeriesDocument=after;acceptedSeriesPaths=files;acceptedSeriesHashes=hashes;acceptedSeriesFileStamps=stamps;
        performanceEvent("records_certified",acceptedPerformanceIdentity());
        if(resultPort){const QSignalBlocker blocker(resultPort);for(const auto &p:acceptedReadouts())resultPort->addItem(tr("%1 | %2 | %3").arg(p.portId,readoutModeName(p.mode),p.unit),p.portId);resultPort->setToolTip(resultPort->currentText());resultPort->setEnabled(resultPort->count()>0);resultReference->setEnabled(resultPort->count()>0);populateResultQuantities();}
        return acceptedNumericSeries&&resultPlot&&!resultPlot->dataset().traces.isEmpty();
    }catch(const std::exception &error){clearAcceptedSeries(QString::fromUtf8(error.what()));status->setText(QString::fromUtf8(error.what()));return false;}catch(...){clearAcceptedSeries(tr("Accepted history failed native qualification."));return false;}
}
bool PiezoWorkbenchWidget::exportAcceptedTimeSeriesFromTrace(const PiezoResultSeries::TraceProof &proof,const QString &path) {
    beginPerformanceRequest("csv",{{"baseModelPath",proof.baseModelPath},{"tracePath",proof.tracePath},{"destination",path},{"origin","trusted-fixture-authority"}});
    QJsonObject before;RModel model;if(!savedSourceSnapshot(&before,&model))return false;if(targetsSelectedDocument(session,path)){status->setText(tr("Choose a separate CSV file; the current model file is protected."));return false;}
    try{const auto fingerprint=sourceFingerprint(model);requireResult(fingerprint==proof.expectedSourceFingerprint,tr("Accepted history belongs to another saved experiment."));
        const auto finalAuthority=[&]{QJsonObject after;RModel current;requireResult(savedSourceSnapshot(&after,&current)&&sourceFingerprint(current)==fingerprint,tr("Source changed during accepted history CSV export."));for(const auto &key:{"documentId","sessionEpoch","documentRevision","project","draft"})requireResult(before.value(key)==after.value(key),tr("Source changed during accepted history CSV export."));const auto pending=QDir(QFileInfo(proof.tracePath).absolutePath()).filePath("pending-publication.json");requireResult(!QFileInfo::exists(pending)&&!QFileInfo(pending).isSymLink(),tr("Accepted history has unresolved PENDING authority."));};
        finalAuthority();bindPerformanceTrace(proof,before);QString error;RPiezoArtifactService::Report report;const bool valid=RPiezoArtifactService::writeAcceptedTraceTimeSeries(proof,path,&error,&report,finalAuthority);if(!valid)status->setText(error);else performanceEvent("csv_committed",{{"family",report.modelFamily==QStringLiteral("axialRod")?QStringLiteral("axial"):QStringLiteral("layered")},{"modelFamily",report.modelFamily},{"sourceFingerprint",report.sourceFingerprint},{"totalRecords",double(report.recordCount)},{"firstTimeS",report.firstTime},{"lastTimeS",report.lastTime},{"continuousFE",report.continuousFE},{"destination",path}});return valid;
    }catch(const std::exception &error){status->setText(QString::fromUtf8(error.what()));return false;}catch(...){status->setText(tr("Accepted history failed native qualification."));return false;}
}
void PiezoWorkbenchWidget::previewRun() {
    if(savedRecipe.isEmpty()||recipe!=savedRecipe){invalidatePreview(tr("Apply the whole draft before running the saved experiment."));return;}
    // This is a proposed, unexecuted run. Trusted Confirm accepts the warnings
    // actually rendered from native validation before any job is submitted.
    requestPreview("analysis.run",{{"warningsAccepted",true}});
}
void PiezoWorkbenchWidget::requestPreview(const QString &command,const QJsonObject &arguments) {
    invalidatePreview(tr("Preparing complete native preview..."));const auto document=currentDocument();
    const auto bad=invalidRecipeValuePath(recipe);if(!bad.isEmpty()){status->setText(tr("Null or non-finite recipe value at %1. Correct the declaration before preview or run.").arg(bad));return;}
    for(const auto *grid:findChildren<QTableWidget*>())if(grid->property("invalidNumericEntry").toBool()){status->setText(tr("Correct invalid numeric matrix entries before preview or run."));return;}
    if(!forceDirectionsValid()){status->setText(tr("A force channel has an invalid node, direction or unit for this source frame. Edit its binding before preview or run."));return;}
    if(!externalSourcesCurrent()){status->setText(tr("External force source or dependency changed. Reload its files explicitly, then preview again."));return;}
    if(document.isEmpty()){status->setText(tr("Select one available native document."));return;}
    if(arguments.isEmpty()){status->setText(tr("No supported experiment draft."));return;}
    auto *host=session->getModelCommandHost();
    const auto response=host->handle(request("command.preview",{{"target",QJsonObject{{"documentId",document.value("documentId")},{"sessionEpoch",document.value("sessionEpoch")}}},
        {"expectedDocumentRevision",document.value("documentRevision")},{"command",command},{"commandVersion",1},{"arguments",arguments}}));
    if(response.value("status")!=QJsonValue("preview")){
        status->setText(response.value("message").toString());
        const auto warnings=response.value("warnings").toArray();if(!warnings.isEmpty()){QStringList texts;for(const auto &w:warnings)texts.append(w.isString()?w.toString():w.toObject()["message"].toString());status->setText(status->text()+"\n"+texts.join("\n"));}return;
    }
    approvedPreview=response.value("result").toObject();previewDocument=document;previewWorkspace=host->projectWorkspace();previewCommand=command;
    showPreview(approvedPreview);applyButton->setEnabled(hasCurrentPreview()&&(command==QStringLiteral("analysis.run")||
        (approvedPreview.value("normalizedArguments").isObject()&&!approvedPreview.value("normalizedArguments").toObject().isEmpty())));
}
namespace {
QString piezoPreviewLabel(const QString &key) {
    static const QHash<QString,QString> labels{
        {"physicalElementId","Physical element ID"},{"physicalNodeId","Physical node ID"},
        {"materialId","Material ID"},{"portId","Port ID"},{"channelId","Force channel ID"},
        {"dof","Physical DOF"},{"Cf","Feedback capacitance"},{"Qref","Reference charge"},
        {"VoutBias","Output bias"},{"initialValue","Initial electrical boundary value"},
        {"rayleighAlpha","Rayleigh alpha"},{"rayleighBeta","Rayleigh beta"},
        {"uniformRatio","Uniform damping ratio"},{"ratios","Damping ratios"},
        {"dt","FE time step"},{"steps","FE steps"},{"frequencyHz","Frequency"},
        {"startFrequencyHz","Start frequency"},{"endFrequencyHz","End frequency"},
        {"phaseRad","Phase"},{"lowerZ","Lower layer coordinate"},{"upperZ","Upper layer coordinate"},
        {"elasticityOrCompliance","Elasticity / compliance matrix"},{"coupling","Piezoelectric coupling matrix"},
        {"permittivity","Permittivity matrix"},{"importedMatrix","Imported damping matrix"},
        {"sourceSha256","Source SHA-256"},{"sourcePath","Source path"},{"dependencyFiles","Dependency files"},
        {"initialDisplacements","Initial displacements"},{"initialVelocities","Initial velocities"},
        {"backgroundForces","Background forces"},{"externalFunctions","External force functions"},
        {"readout","Electrical readout"},{"role","Port role"},{"kind","Type"},
        {"form","Material data form"},{"density","Density"},{"area","Cross-section area"},
        {"thickness","Thickness"},{"width","Width"},{"length","Length"},{"origin","Frame origin"},
        {"ex","Frame x basis"},{"ey","Frame y basis"},{"ez","Frame z basis"},
        {"initialTension","Initial cable tension"},{"voltageLower","Lower actuator voltage bound"},
        {"voltageUpper","Upper actuator voltage bound"},{"deltaVoltageLimits","Actuator voltage change limit"},
        {"value","Value"},{"amplitude","Amplitude"},
        {"startTime","Start time"},{"duration","Duration"},{"riseTime","Rise time"},
        {"holdTime","Hold time"},{"fallTime","Fall time"},{"interpolation","Interpolation"},
        {"outOfRange","Out-of-range behavior"},{"expression","Expression"},
        {"available","Available"},{"samplingPeriod","Control sampling period"},
        {"controlEveryFESteps","Control interval in FE steps"}
    };
    const auto known=labels.constFind(key);
    if(known!=labels.cend())return known.value();
    QString label=key;
    for(int i=label.size()-1;i>0;--i)
        if(label[i].isUpper()&&(label[i-1].isLower()||label[i-1].isDigit()))label.insert(i,' ');
    label.replace('_',' ');
    if(!label.isEmpty())label[0]=label[0].toUpper();
    return label;
}

QString piezoPreviewEntityKey(const QString &schemaPath,const QJsonObject &entry,int index) {
    const QString collection=schemaPath.section('/',-1);
    const auto number=[](const QJsonValue &value){return QString::number(value.toDouble(),'g',15);};
    if(collection=="members"&&entry.contains("physicalElementId"))return "member:"+number(entry.value("physicalElementId"));
    if(collection=="layers"){
        const auto ordinal=entry.value("ordinal");
        return "layer:"+(ordinal.isDouble()?number(ordinal):ordinal.toString(QString::number(index+1)));
    }
    if(collection=="materials"&&entry.contains("materialId"))return "material:"+entry.value("materialId").toString();
    if(collection=="ports"&&entry.contains("portId"))return "port:"+entry.value("portId").toString();
    if(collection=="supports"&&entry.contains("physicalNodeId"))
        return "support:"+number(entry.value("physicalNodeId"))+":"+entry.value("dof").toString();
    if((collection=="backgroundForces"||collection=="initialDisplacements"||collection=="initialVelocities"||
        (collection=="nodes"&&schemaPath.contains("/workpoint/")))&&entry.contains("physicalNodeId"))
        return "node:"+number(entry.value("physicalNodeId"));
    if(collection=="channels"&&entry.contains("channelId"))return "channel:"+entry.value("channelId").toString();
    if(collection=="outputs"&&entry.contains("id"))return "output:"+entry.value("id").toString();
    if(collection=="externalFunctions"&&entry.contains("sourcePath"))
        return "external:"+entry.value("functionName").toString()+":"+entry.value("sourcePath").toString();
    if(entry.contains("physicalElementId"))return "element:"+number(entry.value("physicalElementId"));
    if(entry.contains("physicalNodeId"))return "node:"+number(entry.value("physicalNodeId"));
    if(entry.contains("materialId"))return "material:"+entry.value("materialId").toString();
    if(entry.contains("portId"))return "port:"+entry.value("portId").toString();
    if(entry.contains("channelId"))return "channel:"+entry.value("channelId").toString();
    if(entry.contains("id"))return "id:"+entry.value("id").toString();
    return "ordinal:"+QString::number(index);
}

QString piezoPreviewEntityLabel(const QString &schemaPath,const QJsonObject &entry,int index) {
    const QString collection=schemaPath.section('/',-1);
    const auto number=[](const QJsonValue &value){return QString::number(value.toDouble(),'g',15);};
    if(collection=="members"&&entry.contains("physicalElementId"))
        return QString("Member (physical element ID %1)").arg(number(entry.value("physicalElementId")));
    if(collection=="layers"){
        const auto ordinal=entry.value("ordinal");
        return QString("Layer ordinal %1").arg(ordinal.isDouble()?number(ordinal):ordinal.toString(QString::number(index+1)));
    }
    if(collection=="materials"&&entry.contains("materialId"))
        return QString("Material %1").arg(entry.value("materialId").toString());
    if(collection=="ports"&&entry.contains("portId"))
        return QString("Electrical port %1").arg(entry.value("portId").toString());
    if(collection=="supports"&&entry.contains("physicalNodeId"))
        return QString("Support at physical node ID %1 / %2").arg(number(entry.value("physicalNodeId")),entry.value("dof").toString());
    if((collection=="backgroundForces"||collection=="initialDisplacements"||collection=="initialVelocities"||
        (collection=="nodes"&&schemaPath.contains("/workpoint/")))&&entry.contains("physicalNodeId"))
        return QString("Physical node ID %1").arg(number(entry.value("physicalNodeId")));
    if(collection=="channels"&&entry.contains("channelId")){
        const QString channel=entry.value("channelId").toString();
        const auto parts=channel.split(':');
        if(parts.size()>=3&&parts.first()=="node")
            return QString("Force channel %1 (physical node ID %2, DOF %3)").arg(channel,parts[1],parts[2]);
        return QString("Force channel %1").arg(channel);
    }
    if(collection=="outputs"&&entry.contains("id"))
        return QString("Measurement %1 at physical node ID %2").arg(entry.value("id").toString(),number(entry.value("physicalNodeId")));
    if(collection=="externalFunctions"&&entry.contains("sourcePath"))
        return QString("External function %1 (source %2)").arg(entry.value("functionName").toString(),entry.value("sourcePath").toString());
    return QString("%1 %2").arg(piezoPreviewLabel(collection)).arg(index+1);
}

QString piezoPreviewContainerUnit(const QString &key,const QJsonObject &parent,const QString &inherited) {
    if(key=="permittivity")return "F/m";
    if(key=="elasticityOrCompliance")
        return parent.value("form").toString()=="stressCharge"?"Pa (cE form)":"1/Pa (sE form)";
    if(key=="coupling")
        return parent.value("form").toString()=="stressCharge"?"C/m^2 (e form)":"m/V (d form)";
    if(key=="importedMatrix")return "unit depends on each generalized-coordinate pair";
    if(key=="kp"||key=="ki"||key=="kd"||key=="bias")
        return "unit depends on bound input/output quantities; recipe has no per-entry unit";
    return inherited;
}

QString piezoPreviewPageTitle(const QString &schemaPath) {
    const QString rootKey=schemaPath.section('/',2,2);
    if(schemaPath.contains("/design/outputs")||rootKey=="ports"||rootKey=="supports")
        return "Sensors / electrodes / supports";
    if(rootKey=="family"||rootKey=="frame"||rootKey=="members"||rootKey=="materials"||
       rootKey=="template"||rootKey=="replaceLegacyConditions"||rootKey=="units")
        return "Source / devices / materials";
    if(rootKey=="damping"||rootKey=="design")return "Modes / damping / export";
    if(rootKey=="controller")return "Controllers / state source";
    if(rootKey=="excitation"||rootKey=="backgroundForces"||rootKey=="initialDisplacements"||
       rootKey=="initialVelocities"||rootKey=="workpoint"||rootKey=="dt"||rootKey=="steps")
        return "Excitation / initial state";
    return "Unclassified recipe fields";
}

QString piezoPreviewUnit(const QString &schemaPath,const QString &component,const QString &inherited,const QJsonObject &parent) {
    const QString leaf=schemaPath.section('/',-1);
    bool numericIndex=false;leaf.toInt(&numericIndex);
    const QString key=numericIndex?schemaPath.section('/',-2,-2):leaf;
    if(key=="physicalElementId"||key=="physicalNodeId"||key=="materialId"||key=="portId"||key=="channelId"||key=="id")return "ID";
    if(key=="ordinal")return "ordinal";
    if(key=="steps"||key=="controlEveryFESteps")return "steps";
    if(key=="modeIndices")return "zero-based mode index";
    if(key=="length"||key=="origin")return "m";
    if(key=="ex"||key=="ey"||key=="ez")return "dimensionless";
    if(key=="initialTension")return "N";
    if(key=="voltageLower"||key=="voltageUpper"||key=="deltaVoltageLimits")return "V";
    if(key=="amplifierPolarity"||key=="polarity"||key=="available"||key=="cacheExactTime")return "dimensionless";
    if(key=="rayleighAlpha")return "1/s";
    if(key=="rayleighBeta")return "s";
    if(key=="uniformRatio"||key=="ratios"||key=="ratio"||key=="dampingRatio")return "dimensionless";
    if(key=="dt"||key=="time"||key=="startTime"||key=="duration"||key=="riseTime"||key=="holdTime"||key=="fallTime"||key=="samplingPeriod")return "s";
    if(key=="frequencyHz"||key=="startFrequencyHz"||key=="endFrequencyHz"||key=="frequency")return "Hz";
    if(key=="phaseRad")return "rad";
    if(key=="area")return "m^2";
    if(key=="lowerZ"||key=="upperZ"||key=="thickness"||key=="width")return "m";
    if(key=="density")return "kg/m^3";
    if(key=="Cf")return "F";
    if(key=="Qref"||key=="Q0")return "C";
    if(key=="VoutBias"||key=="V0")return "V";
    if(key=="initialValue"){
        const QString role=parent.value("role").toString();
        const QString readout=parent.value("readout").toString();
        if(role=="actuator"||role=="actuatorSensor")return "V";
        if(role=="sensor"&&readout=="openCircuitVoltage")return "C";
        if(role=="sensor")return "V";
        return "unit unresolved for unknown electrical boundary role";
    }
    if(key=="permittivity")return "F/m";
    if(key=="elasticityOrCompliance"||key=="coupling"||key=="importedMatrix")
        return inherited.isEmpty()?"unit depends on matrix form / coordinate pair":inherited;
    if(key=="value"||key=="amplitude"||key=="values"||key=="q0"||key=="f0"){
        if(schemaPath.contains("/initialDisplacements/")||(schemaPath.contains("/workpoint/nodes/")&&schemaPath.contains("/q0/")))
            return component.startsWith("R")?"rad":"m";
        if(schemaPath.contains("/initialVelocities/"))return component.startsWith("R")?"rad/s":"m/s";
        if(schemaPath.contains("/backgroundForces/")||(schemaPath.contains("/workpoint/nodes/")&&schemaPath.contains("/f0/")))
            return component.startsWith("R")?"N m":"N";
        if(schemaPath.contains("/supports/")){
            const QString dof=parent.value("dof").toString();
            return dof.startsWith("R")?"rad":"m";
        }
        if(schemaPath.contains("/workpoint/ports/")){
            if(schemaPath.endsWith("/V0"))return "V";
            if(schemaPath.endsWith("/Q0"))return "C";
        }
        if(!inherited.isEmpty())return inherited;
        if(schemaPath.contains("/excitation/"))return "force unit not declared by this recipe entry";
        return "unit not declared by recipe schema";
    }
    if(key=="youngsModulus"||key=="elasticModulus")return "Pa";
    if(key=="stiffness")return "N/m (translation) or N m/rad (rotation)";
    if(key=="mass")return "kg";
    if(key=="unit")return {};
    if(!inherited.isEmpty())return inherited;
    return "unit not declared by recipe schema";
}

QString piezoPreviewEnum(const QString &key,const QString &value) {
    if(key=="readout"){
        static const QHash<QString,QString> readouts{
            {"none","None"},{"openCircuitVoltage","Open-circuit voltage"},{"shortCircuitCharge","Short-circuit charge"},
            {"idealChargeAmplifier","Ideal charge amplifier"},{"actuatorCharge","Actuator charge"}
        };
        return readouts.value(value,value);
    }
    if(key=="role"&&value=="actuatorSensor")return "Actuator + sensor";
    if(key=="form"&&value=="stressCharge")return "Stress-charge form";
    if(key=="form"&&value=="strainCharge")return "Strain-charge form";
    if(key=="kind"||key=="family"||key=="quantity"||key=="role"||key=="sourceKind"||key=="interpolation"||key=="outOfRange"){
        if(value=="PI"||value=="PD"||value=="PID"||value=="LQR"||value=="MPC")return value;
        QString label=value;label.replace('_',' ');
        for(int i=label.size()-1;i>0;--i)if(label[i].isUpper()&&label[i-1].isLower())label.insert(i,' ');
        if(!label.isEmpty())label[0]=label[0].toUpper();
        return label;
    }
    return value;
}

QString piezoPreviewValue(const QJsonValue &value,const QString &unit,const QString &schemaPath) {
    QString text;
    if(value.isUndefined())text="Not configured";
    else if(value.isNull())text="Not set";
    else if(value.isBool())text=value.toBool()?"true":"false";
    else if(value.isDouble())text=QString::number(value.toDouble(),'g',15);
    else if(value.isString())text=value.toString().isEmpty()?"(empty string)":piezoPreviewEnum(schemaPath.section('/',-1),value.toString());
    else if(value.isArray()){
        const auto array=value.toArray();QStringList entries;
        for(int i=0;i<array.size();++i)entries.append(piezoPreviewValue(array[i],unit,schemaPath+"/"+QString::number(i)));
        text=array.isEmpty()?"[]":"["+entries.join(", ")+"]";
        if(array.isEmpty()&&!unit.isEmpty())text+=" ["+unit+"]";
    }else if(value.isObject()){
        const auto object=value.toObject();QStringList entries;auto keys=object.keys();std::sort(keys.begin(),keys.end());
        QString objectUnit=unit;
        if(object.contains("unit")&&object.value("unit").isString())objectUnit=object.value("unit").toString();
        if(object.contains("dof")&&objectUnit.isEmpty())objectUnit=object.value("dof").toString().startsWith("R")?"rad":"m";
        for(const auto &key:keys){
            const QString childPath=schemaPath+"/"+key;
            const QString childInherited=piezoPreviewContainerUnit(key,object,objectUnit);
            const QString childUnit=piezoPreviewUnit(childPath,{},childInherited,object);
            entries.append(piezoPreviewLabel(key)+": "+piezoPreviewValue(object.value(key),childUnit,childPath));
        }
        text=object.isEmpty()?"{}":"{"+entries.join("; ")+"}";
    }
    if(value.isDouble()&&!unit.isEmpty())text+=" ["+unit+"]";
    return text;
}

using PiezoPreviewAddRow=std::function<void(const QString&,const QString&,const QString&)>;
void piezoPreviewDiff(const QJsonValue &before,bool beforeExists,const QJsonValue &proposed,bool proposedExists,
                     const QString &schemaPath,const QString &displayPath,const QString &beforeUnit,const QString &proposedUnit,
                     const QString &component,const QJsonObject &beforeParent,const QJsonObject &proposedParent,
                     const PiezoPreviewAddRow &add,int &changedFields) {
    if(beforeExists&&proposedExists&&before==proposed){
        const QString oldUnit=piezoPreviewUnit(schemaPath,component,beforeUnit,beforeParent);
        const QString newUnit=piezoPreviewUnit(schemaPath,component,proposedUnit,proposedParent);
        if(oldUnit==newUnit)return;
    }
    const bool beforeObject=beforeExists&&before.isObject(),proposedObject=proposedExists&&proposed.isObject();
    if((beforeObject||!beforeExists)&&(proposedObject||!proposedExists)){
        const auto oldObject=beforeObject?before.toObject():QJsonObject{};
        const auto newObject=proposedObject?proposed.toObject():QJsonObject{};
        QStringList keys=oldObject.keys();
        for(const auto &key:newObject.keys())if(!keys.contains(key))keys.append(key);
        std::sort(keys.begin(),keys.end());
        if(keys.isEmpty()){
            ++changedFields;
            add(piezoPreviewPageTitle(schemaPath),displayPath.isEmpty()?"Recipe object":displayPath,
                QString("Before: %1\nProposed: %2").arg(beforeExists?"(empty object)":"Not configured",proposedExists?"(empty object)":"Not configured"));
            return;
        }
        QString oldUnit=beforeUnit,newUnit=proposedUnit;
        if(beforeObject&&oldObject.contains("unit")&&oldObject.value("unit").isString())oldUnit=oldObject.value("unit").toString();
        if(proposedObject&&newObject.contains("unit")&&newObject.value("unit").isString())newUnit=newObject.value("unit").toString();
        if(beforeObject&&oldObject.contains("dof")&&oldUnit.isEmpty())oldUnit=oldObject.value("dof").toString().startsWith("R")?"rad":"m";
        if(proposedObject&&newObject.contains("dof")&&newUnit.isEmpty())newUnit=newObject.value("dof").toString().startsWith("R")?"rad":"m";
        for(const auto &key:keys){
            const QString oldChildUnit=piezoPreviewContainerUnit(key,oldObject,oldUnit);
            const QString newChildUnit=piezoPreviewContainerUnit(key,newObject,newUnit);
            const QString label=displayPath.isEmpty()?piezoPreviewLabel(key):displayPath+" / "+piezoPreviewLabel(key);
            piezoPreviewDiff(oldObject.value(key),oldObject.contains(key),newObject.value(key),newObject.contains(key),
                             schemaPath+"/"+key,label,oldChildUnit,newChildUnit,{},oldObject,newObject,add,changedFields);
        }
        return;
    }
    const bool beforeArray=beforeExists&&before.isArray(),proposedArray=proposedExists&&proposed.isArray();
    if((beforeArray||!beforeExists)&&(proposedArray||!proposedExists)){
        const auto oldArray=beforeArray?before.toArray():QJsonArray{};
        const auto newArray=proposedArray?proposed.toArray():QJsonArray{};
        if(oldArray.isEmpty()&&newArray.isEmpty()){
            ++changedFields;
            add(piezoPreviewPageTitle(schemaPath),displayPath.isEmpty()?"Recipe list":displayPath,
                QString("Before: %1\nProposed: %2").arg(beforeExists?"(empty list)":"Not configured",proposedExists?"(empty list)":"Not configured"));
            return;
        }
        const QString collection=schemaPath.section('/',-1);
        const bool stableIdentity=collection=="members"||collection=="layers"||collection=="materials"||collection=="ports"||
            collection=="supports"||collection=="backgroundForces"||collection=="initialDisplacements"||collection=="initialVelocities"||
            collection=="channels"||collection=="outputs"||collection=="externalFunctions"||
            (collection=="nodes"&&schemaPath.contains("/workpoint/"));
        const bool vectorField=(collection=="values"||collection=="q0"||collection=="f0")&&
            (schemaPath.contains("/initialDisplacements/")||schemaPath.contains("/initialVelocities/")||
             schemaPath.contains("/backgroundForces/")||schemaPath.contains("/workpoint/nodes/"));
        const QStringList dofs{"UX","UY","UZ","RX","RY","RZ"};
        QList<int> matchedNew;
        QStringList oldOrder,newOrder;
        if(stableIdentity){
            for(int i=0;i<oldArray.size();++i)oldOrder.append(piezoPreviewEntityLabel(schemaPath,oldArray[i].toObject(),i));
            for(int i=0;i<newArray.size();++i)newOrder.append(piezoPreviewEntityLabel(schemaPath,newArray[i].toObject(),i));
        }
        for(int i=0;i<oldArray.size();++i){
            const auto oldEntry=oldArray[i];int found=-1;
            if(i<newArray.size()&&!matchedNew.contains(i)&&(!stableIdentity||piezoPreviewEntityKey(schemaPath,newArray[i].toObject(),i)==piezoPreviewEntityKey(schemaPath,oldEntry.toObject(),i)))found=i;
            if(stableIdentity&&found<0){
                const QString oldKey=piezoPreviewEntityKey(schemaPath,oldEntry.toObject(),i);
                for(int j=0;j<newArray.size();++j)if(!matchedNew.contains(j)&&piezoPreviewEntityKey(schemaPath,newArray[j].toObject(),j)==oldKey){found=j;break;}
            }
            const auto oldObject=oldEntry.toObject();
            QString entity=vectorField&&i<dofs.size()?dofs[i]:piezoPreviewEntityLabel(schemaPath,found>=0?newArray[found].toObject():oldObject,i);
            const QString identity=stableIdentity?(found>=0?piezoPreviewEntityKey(schemaPath,newArray[found].toObject(),found):piezoPreviewEntityKey(schemaPath,oldObject,i)):QString::number(i);
            if(found>=0)matchedNew.append(found);
            const QString oldDisplay=displayPath.isEmpty()?entity:displayPath+" / "+entity;
            const QString childComponent=vectorField&&i<dofs.size()?dofs[i]:component;
            piezoPreviewDiff(oldEntry,true,found>=0?newArray[found]:QJsonValue(QJsonValue::Undefined),found>=0,
                             schemaPath+"/"+identity,oldDisplay,beforeUnit,found>=0?proposedUnit:QString(),childComponent,oldObject,
                             found>=0?newArray[found].toObject():QJsonObject{},add,changedFields);
        }
        for(int i=0;i<newArray.size();++i)if(!matchedNew.contains(i)){
            const auto newEntry=newArray[i];const auto newObject=newEntry.toObject();
            const QString identity=stableIdentity?piezoPreviewEntityKey(schemaPath,newObject,i):QString::number(i);
            const QString entity=vectorField&&i<dofs.size()?dofs[i]:piezoPreviewEntityLabel(schemaPath,newObject,i);
            const QString childComponent=vectorField&&i<dofs.size()?dofs[i]:component;
            piezoPreviewDiff(QJsonValue(QJsonValue::Undefined),false,newEntry,true,schemaPath+"/"+identity,
                             displayPath.isEmpty()?entity:displayPath+" / "+entity,QString(),proposedUnit,childComponent,QJsonObject{},newObject,add,changedFields);
        }
        if(stableIdentity&&oldOrder!=newOrder){
            ++changedFields;
            add(piezoPreviewPageTitle(schemaPath),displayPath.isEmpty()?piezoPreviewLabel(collection)+" / Order":displayPath+" / Order",
                QString("Before: %1\nProposed: %2").arg(oldOrder.join(", "),newOrder.join(", ")));
        }
        return;
    }
    const QString field=schemaPath.section('/',-1);
    const QString oldValue=piezoPreviewValue(beforeExists?before:QJsonValue(QJsonValue::Undefined),
        piezoPreviewUnit(schemaPath,component,beforeUnit,beforeParent),schemaPath);
    const QString newValue=piezoPreviewValue(proposedExists?proposed:QJsonValue(QJsonValue::Undefined),
        piezoPreviewUnit(schemaPath,component,proposedUnit,proposedParent),schemaPath);
    ++changedFields;
    add(piezoPreviewPageTitle(schemaPath),displayPath.isEmpty()?piezoPreviewLabel(field):displayPath,
        QString("Before: %1\nProposed: %2").arg(oldValue,newValue));
}
}

void PiezoWorkbenchWidget::showPreview(const QJsonObject &result) {
    auto *expandPreview=changes->parentWidget()->findChild<QPushButton*>("piezoExpandPreview");
    changes->setMaximumHeight(120);changes->setMinimumHeight(104);
    if(expandPreview){expandPreview->setChecked(false);expandPreview->hide();}
    changes->setRowCount(0);changes->show();if(auto *panel=changes->parentWidget())panel->show();
    const auto revealPreview=[this]{
        if(changes->rowCount()>0){changes->setCurrentCell(0,0);changes->setFocus(Qt::OtherFocusReason);}
        QTimer::singleShot(0,this,[this]{
            if(!changes->isVisible())return;
            QWidget *previewPanel=changes->parentWidget();
            for(QWidget *parent=changes->parentWidget();parent;parent=parent->parentWidget())
                if(auto *scroll=qobject_cast<QScrollArea*>(parent))scroll->ensureWidgetVisible(previewPanel,0,0);
        });
    };
    const auto add=[this](const QString &key,const QString &value){int row=changes->rowCount();changes->insertRow(row);changes->setItem(row,0,new QTableWidgetItem(key));changes->setItem(row,1,new QTableWidgetItem(value));};
    const auto native=result.value("nativePreview").toObject();
    auto report=result.value("report").toObject();if(report.isEmpty())report=native.value("report").toObject();
    if(previewCommand=="analysis.run"){
        add(tr("Operation"),tr("Submit one native solver job for the unchanged saved experiment"));
        const auto modelName=previewDocument.value("modelName").toString();
        const auto modelRevision=previewDocument.value("documentRevision");
        const QString savedSource=modelRevision.isDouble()
            ?tr("%1 (revision %2)").arg(modelName.isEmpty()?tr("Unnamed model"):modelName).arg(modelRevision.toDouble(),0,'f',0)
            :tr("%1 (revision unavailable)").arg(modelName.isEmpty()?tr("Unnamed model"):modelName);
        add(tr("Saved model"),savedSource);
        const auto savedSteps=savedRecipe.value("steps");
        const auto savedDt=savedRecipe.value("dt");
        const double stepsNumber=savedSteps.toDouble();
        const double dt=savedDt.toDouble();
        const double duration=stepsNumber*dt;
        if(savedSteps.isDouble()&&savedDt.isDouble()&&std::isfinite(stepsNumber)&&stepsNumber>0&&std::floor(stepsNumber)==stepsNumber&&
           std::isfinite(dt)&&dt>0&&std::isfinite(duration)){
            add(tr("Saved FE setup"),tr("%1 FE steps; dt %2 s; total duration %3 s")
                .arg(QString::number(static_cast<qulonglong>(stepsNumber)),QString::number(dt,'g',17),QString::number(duration,'g',17)));
        }else add(tr("Saved FE setup"),tr("Unavailable in the authoritative saved experiment."));
        const QString controllerKind=savedRecipe.value("controller").toObject().value("kind").toString();
        add(tr("Saved controller"),controllerKind.isEmpty()?tr("Unavailable in the authoritative saved experiment."):controllerKind);
        add(tr("Accepted history storage"),tr("Every accepted FE step and the initial state are stored in a compact history, with periodic and final full MODEL checkpoints. Result output frequency does not thin this history."));
        auto validation=result.value("validation").toObject();if(validation.isEmpty())validation=native.value("validation").toObject();
        for(const auto &w:validation.value("warnings").toArray())add(tr("Solver warning"),w.isString()?w.toString():w.toObject()["message"].toString());
        status->setText(tr("Review native solver validation and all displayed warnings. Confirm accepts these warnings and submits one native job."));
        applyButton->setText(tr("Confirm run and displayed warnings"));changes->resizeRowsToContents();
        if(expandPreview)expandPreview->setVisible(previewContentHeight(changes)>120);
        revealPreview();return;
    }
    add(tr("Replacement"),tr("One complete experiment; one Undo history group"));
    for(const auto &key:QStringList{"totalMass","fullDofs","freeDofs","members","certification"})
        if(report.contains(key))add(key,report[key].isString()?report[key].toString():QString::number(report[key].toDouble(),'g',12));
    int groupNumber=0;for(const auto &value:report.value("automaticLineGrouping").toArray()){
        const auto group=value.toObject();QStringList memberIds;
        for(const auto &id:group.value("memberPhysicalIds").toArray())memberIds.append(QString::number(static_cast<qulonglong>(id.toDouble())));
        add(tr("Auto-separated member properties"),tr("Group %1: %2 members; physical element IDs: %3; declared area %4 m²; %5")
            .arg(++groupNumber).arg(memberIds.size()).arg(memberIds.join(", ")).arg(group.value("area").toDouble(),0,'g',17)
            .arg(group.value("retainedParentIdentity").toBool()?tr("original group binding retained"):tr("new group binding assigned automatically")));
        changes->item(changes->rowCount()-1,1)->setData(Qt::UserRole,group.toVariantMap());
    }
    const auto design=report.value("design").toObject();
    if(!design.isEmpty()){
        add(tr("Qualified modal design"),tr("%1 selected modes; %2 certified clusters")
            .arg(design.value("selectedModes").toArray().size()).arg(design.value("clusters").toArray().size()));
        double residual=0;for(const auto &v:design.value("modalResiduals").toArray())residual=std::max(residual,v.toDouble());
        add(tr("Maximum eigen residual"),QString::number(residual,'g',10));
        add(tr("Mass orthogonality error"),QString::number(design.value("massOrthogonalityError").toDouble(),'g',10));
        auto indices=design.value("selectedModes").toArray(),omega=design.value("omega").toArray();for(int i=0;i<indices.size()&&i<omega.size();++i)
            add(tr("Mode %1 frequency").arg(indices[i].toInt()+1),tr("%1 Hz").arg(omega[i].toDouble()/(2*std::acos(-1.)),0,'g',10));
        if(design.contains("samplingPeriod"))add(tr("Control sampling period"),tr("%1 s").arg(design.value("samplingPeriod").toDouble(),0,'g',10));
        const auto controller=design.value("controller").toObject();
        if(controller.contains("kind"))add(tr("Certified controller"),controller.value("kind").toString());
        if(controller.contains("riccatiRelativeResidual"))add(tr("Riccati relative residual"),QString::number(controller.value("riccatiRelativeResidual").toDouble(),'g',10));
        if(controller.contains("horizon"))add(tr("MPC horizon"),QString::number(controller.value("horizon").toInt()));
        if(controller.contains("qualification"))add(tr("Controller qualification"),controller.value("qualification").toString());
        QStringList poles;for(const auto &v:controller.value("closedPoles").toArray()){auto pole=v.toArray();if(pole.size()==2)poles.append(QString("%1 %2 %3i").arg(pole[0].toDouble(),0,'g',7).arg(pole[1].toDouble()<0?"−":"+").arg(std::abs(pole[1].toDouble()),0,'g',7));}
        if(!poles.isEmpty())add(tr("Closed-loop poles"),poles.join(", "));
        const auto force=design.value("forceAware").toObject();if(force.contains("qualification"))add(tr("Force / readout contract"),force.value("qualification").toString());
    }
    const auto removed=report.value("removedLegacyConditions").toArray();add(tr("Legacy conditions replaced"),QString::number(removed.size()));
    for(const auto &v:removed){auto o=v.toObject();add(tr("Removed legacy condition"),tr("%1 / type %2").arg(o.value("kind").toString()).arg(o.value("type").toInt()));}
    for(const auto &v:recipe.value("materials").toArray()){const auto m=v.toObject();add(tr("Material provenance"),m.value("materialId").toString()+": "+m.value("sourceKind").toString()+" / "+m.value("sourceId").toString()+" / "+m.value("notes").toString());}
    const auto normalizedValue=result.value("normalizedArguments");
    const int completeDiffStart=changes->rowCount();
    if(!normalizedValue.isObject()||normalizedValue.toObject().isEmpty()){
        add(tr("Complete recipe diff"),tr("Unavailable: the native preview did not return normalizedArguments. Confirmation is disabled until a complete recipe is available."));
        status->setText(tr("The native preview did not return the normalized recipe. It cannot be confirmed safely; request a fresh preview."));
        applyButton->setEnabled(false);
    }else{
        const auto proposedRecipe=normalizedValue.toObject();
        const bool hasSavedRecipe=!savedRecipe.isEmpty();
        int changedFields=0;
        QHash<QString,QList<QPair<QString,QString>>> fieldsByPage;
        const auto addRecipeField=[&fieldsByPage](const QString &page,const QString &field,const QString &values){
            fieldsByPage[page].append(qMakePair(field,values));
        };
        piezoPreviewDiff(savedRecipe,hasSavedRecipe,proposedRecipe,true,QStringLiteral("/recipe"),QString(),
                         QString(),QString(),QString(),QJsonObject{},QJsonObject{},addRecipeField,changedFields);
        changes->insertRow(completeDiffStart);
        changes->setItem(completeDiffStart,0,new QTableWidgetItem(tr("Complete recipe fields changed")));
        changes->setItem(completeDiffStart,1,new QTableWidgetItem(tr("%1 changed fields; before values come from the saved recipe, proposed values from the native normalized recipe.")
            .arg(changedFields)));
        QStringList pages{QStringLiteral("Source / devices / materials"),QStringLiteral("Sensors / electrodes / supports"),
            QStringLiteral("Modes / damping / export"),QStringLiteral("Controllers / state source"),
            QStringLiteral("Excitation / initial state"),QStringLiteral("Accepted results")};
        if(fieldsByPage.contains(QStringLiteral("Unclassified recipe fields")))
            pages.append(QStringLiteral("Unclassified recipe fields"));
        for(const auto &page:pages){
            const int section=changes->rowCount();changes->insertRow(section);
            auto *title=new QTableWidgetItem(tr("Page: %1").arg(page));
            auto *count=new QTableWidgetItem(tr("%1 changed fields").arg(fieldsByPage.value(page).size()));
            for(auto *item:{title,count}){QFont font=item->font();font.setBold(true);item->setFont(font);item->setBackground(QColor(229,237,247));item->setForeground(QColor(42,61,83));}
            changes->setItem(section,0,title);changes->setItem(section,1,count);
            const auto fields=fieldsByPage.value(page);
            if(fields.isEmpty()){add(tr("No changed recipe fields"),tr("This page has no recipe-owned settings to review."));continue;}
            if(page==QStringLiteral("Unclassified recipe fields"))
                add(tr("Schema mapping required"),tr("These changed recipe fields are outside the six mapped workbench pages and remain explicitly unclassified."));
            for(const auto &field:fields){
                const int row=changes->rowCount();add(field.first,field.second);
                auto *label=changes->item(row,0);auto *values=changes->item(row,1);
                if(label&&(label->text().contains("ID")||label->text().contains("port",Qt::CaseInsensitive)||
                           label->text().contains("channel",Qt::CaseInsensitive)||label->text().contains("material",Qt::CaseInsensitive))){
                    QFont font=label->font();font.setBold(true);label->setFont(font);label->setForeground(QColor(39,83,139));
                    label->setToolTip(tr("Blue labels identify a recipe entity or physical binding."));
                }
                if(values){values->setForeground(QColor(23,105,93));values->setToolTip(tr("Numeric unit annotations are shown in brackets; [ID] marks an identifier."));}
            }
        }
    }

    add(tr("Invalidated"),tr("Previous mass/stiffness/readout, modes, ROM, control design, results and restart"));
    status->setText(normalizedValue.isObject()&&!normalizedValue.toObject().isEmpty()
        ?tr("Review all replacement effects, physical support limits and source provenance; confirm once to apply.")
        :tr("The complete recipe diff is unavailable, so confirmation remains disabled. Request a fresh preview."));
    changes->resizeRowsToContents();if(expandPreview)expandPreview->setVisible(previewContentHeight(changes)>120);revealPreview();
}
void PiezoWorkbenchWidget::applyPreview() {
    if(!hasCurrentPreview()){invalidatePreview(tr("Preview is stale. Preview again before confirming."));return;}
    if(previewCommand!="analysis.run"){
        const auto normalized=approvedPreview.value("normalizedArguments");
        if(!normalized.isObject()||normalized.toObject().isEmpty()){
            invalidatePreview(tr("The complete normalized recipe is unavailable. Preview again before confirming."));
            return;
        }
    }
    auto *host=session->getModelCommandHost();const auto command=previewCommand;const auto previewId=approvedPreview.value("previewId").toString();const auto sourceDocument=previewDocument.value("documentId").toString();const auto selectedBefore=session->getSelectedModelIDs();applyButton->setEnabled(false);
    if(!host->approvePreview(previewId)){invalidatePreview(tr("Approval rejected. Preview again."));return;}
    const auto response=host->handle(request("command.execute",{{"previewId",previewId},{"idempotencyKey",token()}}));
    const bool accepted=response.value("status")==QJsonValue("accepted");
    invalidatePreview(accepted?(command=="analysis.run"?tr("Native solver job submitted."):tr("Whole experiment applied.")):response.value("message").toString());
    if(accepted){const auto result=response.value("result").toObject();receipt->setText(command=="analysis.run"?tr("Run confirmed; native job tracking is available in the solver panel."):
        tr("Complete experiment accepted. History: %1").arg(result.value("history").toString()));
        if(command=="analysis.run"){
            submittedRunJobId=result.value("jobId").toString();
            clearAcceptedSeries(tr("Run confirmed. Waiting for its registered native result to complete."));
            emit runSubmitted(submittedRunJobId);
        }else{
            // A generated mesh gives a previously empty document new entity groups.
            // Keep that same user-selected source selected when the tree rebuild
            // has cleared its former empty-model selection.
            if(selectedBefore.size()==1&&session->getSelectedModelIDs().isEmpty()&&selectedBefore.first()<session->getNModels()&&session->modelCommandStamp(selectedBefore.first()).first==sourceDocument)session->setModelSelected(selectedBefore.first(),true);
            loadedDocument={};refreshContext();emit experimentApplied();}
    }
}
