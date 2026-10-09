#include <rmesh_job_service.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTextStream>
#include <cstdio>
#include <memory>
#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

int main(int argc,char **argv)
{
    QCoreApplication application(argc,argv);
    const QStringList arguments=application.arguments();
    // Match the native tools: one JSON reply on stdout, backend diagnostics on stderr.
    std::fflush(stdout);
#ifdef Q_OS_WIN
    const int jsonFd=_dup(_fileno(stdout));
    FILE *channel=jsonFd<0 ? nullptr : _fdopen(jsonFd,"wb");
    const int redirected=_dup2(_fileno(stderr),_fileno(stdout));
#else
    const int jsonFd=dup(fileno(stdout));
    FILE *channel=jsonFd<0 ? nullptr : fdopen(jsonFd,"w");
    const int redirected=dup2(fileno(stderr),fileno(stdout));
#endif
    if (!channel || redirected<0) return 2;
    std::unique_ptr<FILE,decltype(&std::fclose)> outputGuard(channel,&std::fclose);
    auto finish=[channel](const RMeshJobService::Result &result) {
        const QByteArray bytes=QJsonDocument(result.toJson()).toJson(QJsonDocument::Compact)+'\n';
        std::fwrite(bytes.constData(),1,size_t(bytes.size()),channel); std::fflush(channel);
        return result.valid ? 0 : 3;
    };
    QString action=QStringLiteral("run"),requestPath;
    if (arguments.size()==3 && arguments[1]==QStringLiteral("--request")) requestPath=arguments[2];
    else if (arguments.size()==4 && arguments[2]==QStringLiteral("--request")
             && (arguments[1]==QStringLiteral("prepare") || arguments[1]==QStringLiteral("run")
                 || arguments[1]==QStringLiteral("query")))
    { action=arguments[1]; requestPath=arguments[3]; }
    else
    {
        RMeshJobService::Result result; result.code="usage_error";
        result.message="Require prepare|run|query --request <absolute request.json>; legacy --request runs a frozen job.";
        return finish(result);
    }
    QFile file(requestPath); const QFileInfo info(file);
    if (!info.isAbsolute() || !info.isFile() || info.isSymLink() || info.size()<=0 || info.size()>65536
        || QDir::cleanPath(info.absoluteFilePath())!=QDir::cleanPath(info.canonicalFilePath())
        || !file.open(QIODevice::ReadOnly))
    {
        RMeshJobService::Result result; result.code="invalid_request_file"; result.message="Require a bounded regular request JSON file."; return finish(result);
    }
    const QByteArray requestBytes=file.read(65537);
    if (requestBytes.size()!=file.size() || requestBytes.size()>65536 || file.error()!=QFileDevice::NoError)
    {
        RMeshJobService::Result result; result.code="invalid_request_file";
        result.message="Request read failed or exceeded its bound."; return finish(result);
    }
    QJsonParseError error;
    const auto document=QJsonDocument::fromJson(requestBytes,&error);
    if (error.error!=QJsonParseError::NoError || !document.isObject())
    {
        RMeshJobService::Result result; result.code="invalid_request_file"; result.message="Require one frozen request JSON object."; return finish(result);
    }
    const auto request=document.object();
    const QString actualWorker=QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    const QFileInfo requestedWorker(request.value("workerPath").toString());
    if (!requestedWorker.isAbsolute() || !requestedWorker.isFile() || requestedWorker.isSymLink()
        || QDir::cleanPath(requestedWorker.absoluteFilePath())!=QDir::cleanPath(requestedWorker.canonicalFilePath())
        || requestedWorker.canonicalFilePath()!=actualWorker
        || request.value("workerSha256")!=QJsonValue(RMeshJobService::fileSha256(actualWorker)))
    {
        RMeshJobService::Result result; result.code="worker_changed";
        result.message="Request must bind the actual fixed worker executable and its bytes."; return finish(result);
    }
    if (action==QStringLiteral("prepare"))
    {
        // The file caller owns this capture. This endpoint grants no GUI
        // approval, Session identity, acceptance or solver binding authority.
        if (request.value("runtimeContext").toObject().value("authority")!=QJsonValue("file-owner"))
        {
            RMeshJobService::Result result; result.code="invalid_runtime_context";
            result.message="CLI prepare requires explicit file-owner context."; return finish(result);
        }
        return finish(RMeshJobService::prepare(request));
    }
    if (info.canonicalFilePath()!=QFileInfo(request.value("jobDirectory").toString()+"/request.json").canonicalFilePath())
    {
        RMeshJobService::Result result; result.code="request_path_mismatch"; result.message="Request must be the owned job request.json."; return finish(result);
    }
    if (action==QStringLiteral("query")) return finish(RMeshJobService::queryReconcile(request));
    return finish(RMeshJobService::runWorker(request,QCoreApplication::applicationFilePath()));
}
