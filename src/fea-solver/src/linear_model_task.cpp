#include "linear_model_task.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <rbl_error.h>
#include <rml_file_io.h>
#include <rml_model.h>
#include <rstructural_linear_model.h>
#include <algorithm>
#include <memory>
#include <vector>

namespace {
void require(bool condition,const char *message) {
    if (!condition) throw RError(RError::Type::InvalidInput,R_ERROR_REF,"Stage 9 export: %s",message);
}
QString resolved(const QString &path,bool mustExist) {
    const QFileInfo file(path);
    if (file.exists()) { require(file.isFile(),"Path is not a regular file."); return file.canonicalFilePath(); }
    require(!mustExist && !file.isSymLink(),"Input is missing or destination is a dangling link.");
    const auto parent=file.dir().canonicalPath();
    require(!parent.isEmpty(),"Destination directory must exist.");
    return QDir(parent).filePath(file.fileName());
}
QByteArray hash(const QString &path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly),"Cannot read bound input/output.");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    require(digest.addData(&file),"Cannot hash bound input/output.");
    return digest.result();
}
}
void exportStructuralLinearModel(const QString &input,const QString &output)
{
    const auto source=resolved(input,true),destination=resolved(output,false);
    require(source.compare(destination,Qt::CaseInsensitive)!=0,"Output cannot overwrite the native input.");
    require(QFileInfo(destination).suffix().compare("json",Qt::CaseInsensitive)==0,"Destination must have .json extension.");
    const bool ascii=QFileInfo(source).suffix()==RModel::getDefaultFileExtension(false);
    require(ascii || QFileInfo(source).suffix()==RModel::getDefaultFileExtension(true),"Input must be a native .ram/.rbm model.");
    QStringList paths{source,destination}; paths.sort(Qt::CaseInsensitive);
    std::vector<std::unique_ptr<QLockFile>> locks;
    for (const auto &path:paths) {
        auto lock=std::make_unique<QLockFile>(path+".lock");
        lock->setStaleLockTime(0);
        require(lock->tryLock(0),"Input or output is busy."); locks.push_back(std::move(lock));
    }
    const auto sourceHash=hash(source);
    const bool outputExisted=QFileInfo::exists(destination);
    const auto priorHash=outputExisted ? hash(destination) : QByteArray();
    RFile native(source,ascii ? RFile::ASCII : RFile::BINARY);
    require(native.open(ascii ? QIODevice::ReadOnly|QIODevice::Text : QIODevice::ReadOnly),"Cannot open native model.");
    RFileHeader header;
    if (ascii) RFileIO::readAscii(native,header); else RFileIO::readBinary(native,header);
    require(header.getType()==R_FILE_TYPE_MODEL,"Links are unsupported; input must contain the complete model.");
    native.close();
    RModel model; model.read(source);
    auto bundle=RStructuralLinearModel::build(model);
    auto metadata=bundle["source"].toObject();
    metadata["sha256"]=QString::fromLatin1(sourceHash.toHex());
    metadata["fileName"]=QFileInfo(source).fileName();
    bundle["source"]=metadata;
    const auto bytes=QJsonDocument(bundle).toJson(QJsonDocument::Indented);
    require(bytes.size()<=32*1024*1024,"Linear model exceeds 32 MiB.");
    require(resolved(input,true)==source && resolved(output,false)==destination && hash(source)==sourceHash &&
            QFileInfo::exists(destination)==outputExisted && (!outputExisted || hash(destination)==priorHash),"Input/output changed during assembly; no result published.");
    QSaveFile file(destination);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit(),"Atomic result publication failed.");
}
