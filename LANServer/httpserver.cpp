#include "httpserver.h"
#include "qhttpengine/filesystemhandler.h"
#include "qhttpengine/qobjecthandler.h"
#include "qhttpengine/handler.h"
#include "qhttpengine/qiodevicecopier.h"
#include "qhttpengine/range.h"

#include "Common/zlib.h"
#include "Play/Playlist/playlist.h"
#include "Play/Danmu/common.h"
#include "Play/Danmu/Manager/danmumanager.h"
#include "Play/Danmu/Manager/pool.h"
#include "globalobjects.h"

#include <QCoreApplication>
#include <QMimeDatabase>
namespace
{
    class MediaFileHandler : public QHttpEngine::FilesystemHandler
    {
    public:
        explicit MediaFileHandler(const QHash<QString,QString> *mediaHash,QObject *parent = nullptr):
            FilesystemHandler(parent),mediaHashTable(mediaHash){}

        // FilesystemHandler interface
    protected:
        virtual void process(QHttpEngine::Socket *socket, const QString &path)
        {
            if(path.startsWith("media/"))
            {
                QString mediaId(path.mid(6).trimmed());
                QString mediaPath(mediaHashTable->value(mediaId,""));
                if(mediaPath.isEmpty())
                {
                    socket->writeError(QHttpEngine::Socket::NotFound);
                }
                else
                {
                    processFile(socket, mediaPath);
                }
            }
            else if(path.startsWith("sub/")) // eg. sub/ass/...
            {
                QStringList infoList(path.split('/',QString::SkipEmptyParts));
                if(infoList.count()<3)
                {
                    socket->writeError(QHttpEngine::Socket::BadRequest);
                    return;
                }
                QString mediaPath(mediaHashTable->value(infoList[2],""));
                QFileInfo fi(mediaPath);
                QString subPath=QString("%1/%2.%3").arg(fi.absolutePath(),fi.baseName(),infoList[1]);
                processFile(socket, subPath);
            }
            else
            {
                FilesystemHandler::process(socket,path);
            }
        }
    private:
        const QHash<QString,QString> *mediaHashTable;
        QMimeDatabase database;
        void processFile(QHttpEngine::Socket *socket, const QString &absolutePath)
        {
            // Attempt to open the file for reading
            QFile *file = new QFile(absolutePath);
            if (!file->open(QIODevice::ReadOnly)) {
                delete file;

                socket->writeError(QHttpEngine::Socket::Forbidden);
                return;
            }

            // Create a QIODeviceCopier to copy the file contents to the socket
            QHttpEngine::QIODeviceCopier *copier = new QHttpEngine::QIODeviceCopier(file, socket);
            connect(copier, &QHttpEngine::QIODeviceCopier::finished, copier, &QHttpEngine::QIODeviceCopier::deleteLater);
            connect(copier, &QHttpEngine::QIODeviceCopier::finished, file, &QFile::deleteLater);
            connect(copier, &QHttpEngine::QIODeviceCopier::finished, [socket]() {
                socket->close();
            });

            // Stop the copier if the socket is disconnected
            connect(socket, &QHttpEngine::Socket::disconnected, copier, &QHttpEngine::QIODeviceCopier::stop);

            qint64 fileSize = file->size();

            // Checking for partial content request
            QByteArray rangeHeader = socket->headers().value("Range");
            QHttpEngine::Range range;

            if (!rangeHeader.isEmpty() && rangeHeader.startsWith("bytes=")) {
                // Skiping 'bytes=' - first 6 chars and spliting ranges by comma
                QList<QByteArray> rangeList = rangeHeader.mid(6).split(',');

                // Taking only first range, as multiple ranges require multipart
                // reply support
                range = QHttpEngine::Range(QString(rangeList.at(0)), fileSize);
            }

            // If range is valid, send partial content
            if (range.isValid()) {
                socket->setStatusCode(QHttpEngine::Socket::PartialContent);
                socket->setHeader("Content-Length", QByteArray::number(range.length()));
                socket->setHeader("Content-Range", QByteArray("bytes ") + range.contentRange().toLatin1());
                copier->setRange(range.from(), range.to());
            } else {
                // If range is invalid or if it is not a partial content request,
                // send full file
                socket->setHeader("Content-Length", QByteArray::number(fileSize));
            }

            // Set the mimetype and content length
            socket->setHeader("Content-Type", mimeType(absolutePath));
            socket->writeHeaders();

            // Start the copy
            copier->start();
        }

        QByteArray mimeType(const QString &absolutePath)
        {
            // Query the MIME database based on the filename and its contents
            return database.mimeTypeForFile(absolutePath).name().toUtf8();
        }
    };
}
HttpServer::HttpServer(QObject *parent) : QObject(parent)
{
    MediaFileHandler *handler=new MediaFileHandler(&mediaHash,this);
    handler->setDocumentRoot(QCoreApplication::applicationDirPath()+"/web");
    handler->addRedirect(QRegExp("^$"), "/index.html");

    QHttpEngine::QObjectHandler *apiHandler=new QHttpEngine::QObjectHandler(this);
    apiHandler->registerMethod("playlist", this, &HttpServer::api_Playlist);
    apiHandler->registerMethod("updateTime", this, &HttpServer::api_UpdateTime);
    apiHandler->registerMethod("danmu/v3/", this, &HttpServer::api_Danmu);
    apiHandler->registerMethod("subtitle", this, &HttpServer::api_Subtitle);
    handler->addSubHandler(QRegExp("api/"), apiHandler);

    server = new QHttpEngine::Server(handler,this);
}

HttpServer::~HttpServer()
{
    server->close();
}

QString HttpServer::startServer(qint64 port)
{

   if(server->isListening())return QString();
   server->listen(QHostAddress::AnyIPv4,port);
   genLog(server->errorString().isEmpty()?"Server start":server->errorString());
   return server->errorString();
}

void HttpServer::stopServer()
{
    server->close();
    genLog("Server close");
}

void HttpServer::genLog(const QString &logInfo)
{
    emit showLog(QString("%1%2").arg(QTime::currentTime().toString("[hh:mm:ss]"),logInfo));
}


int HttpServer::compress(const QByteArray &input, QByteArray &output)
{
    int ret;
    unsigned have;
    const int chunkSize=16384;
    z_stream stream;
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    stream.avail_in = 0;
    stream.next_in = Z_NULL;
    ret = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                            MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) return ret;
    unsigned char inBuf[chunkSize];
    unsigned char outBuf[chunkSize];
    QDataStream inStream(input);
    int flush;
    while(!inStream.atEnd())
    {
        stream.avail_in=inStream.readRawData((char *)&inBuf,chunkSize);
        flush = stream.avail_in<chunkSize ? Z_FINISH : Z_NO_FLUSH;
        stream.next_in = inBuf;
        do
        {
            stream.avail_out = chunkSize;
            stream.next_out = outBuf;
            ret = deflate(&stream, flush);
            assert(ret != Z_STREAM_ERROR);
            have = chunkSize - stream.avail_out;
            output.append((const char *)outBuf,have);
        } while (stream.avail_out == 0);
    }
    (void)deflateEnd(&stream);
    return Z_OK ;
}

void HttpServer::api_Playlist(QHttpEngine::Socket *socket)
{
    QMetaObject::invokeMethod(GlobalObjects::playlist,[this](){
        GlobalObjects::playlist->dumpJsonPlaylist(playlistDoc,mediaHash);
    },Qt::BlockingQueuedConnection);
    genLog(QString("[%1]Request:Playlist").arg(socket->peerAddress().toString()));

    QByteArray data = playlistDoc.toJson();
    QByteArray compressedBytes;
    compress(data,compressedBytes);

    socket->setHeader("Content-Length", QByteArray::number(compressedBytes.length()));
    socket->setHeader("Content-Type", "application/json");
    socket->setHeader("Content-Encoding", "gzip");
    socket->writeHeaders();
    socket->write(compressedBytes);
    socket->close();
}

void HttpServer::api_UpdateTime(QHttpEngine::Socket *socket)
{
    bool syncPlayTime=GlobalObjects::appSetting->value("Server/SyncPlayTime",true).toBool();
    if(syncPlayTime)
    {
        QJsonDocument document;
        if (socket->readJson(document))
        {
            genLog(QString("[%1]Request:UpdateTime").arg(socket->peerAddress().toString()));
            QVariantMap data = document.object().toVariantMap();
            QString mediaPath=mediaHash.value(data.value("mediaId").toString());
            int playTime=data.value("playTime").toInt();
            int playTimeState=data.value("playTimeState").toInt();
            QMetaObject::invokeMethod(GlobalObjects::playlist,[mediaPath,playTime,playTimeState](){
                GlobalObjects::playlist->updatePlayTime(mediaPath,playTime,playTimeState);
            },Qt::QueuedConnection);
        }
    }
    socket->close();
}

void HttpServer::api_Danmu(QHttpEngine::Socket *socket)
{ 
    QString poolId=socket->queryString().value("id");
    bool update=(socket->queryString().value("update").toLower()=="true");
    Pool *pool=GlobalObjects::danmuManager->getPool(poolId);
    genLog(QString("[%1]Request:Danmu %2%3").arg(socket->peerAddress().toString(),
                                                   pool?pool->epTitle():"",
                                                   update?", update=true":""));
    QJsonArray danmuArray;
    if(pool)
    {
        if(update)
        {
            QList<QSharedPointer<DanmuComment> > incList;
            pool->update(-1,&incList);
            danmuArray=Pool::exportJson(incList);
        }
        else
        {
            danmuArray=pool->exportJson();
        }
    }
    QJsonObject resposeObj
    {
        {"code", 0},
        {"data", danmuArray},
        {"update",update}
    };
    QByteArray data = QJsonDocument(resposeObj).toJson();
    QByteArray compressedBytes;
    compress(data,compressedBytes);
    socket->setHeader("Content-Length", QByteArray::number(compressedBytes.length()));
    socket->setHeader("Content-Type", "application/json");
    socket->setHeader("Content-Encoding", "gzip");
    socket->writeHeaders();
    socket->write(compressedBytes);
    socket->close();
}

void HttpServer::api_Subtitle(QHttpEngine::Socket *socket)
{
    QString mediaId=socket->queryString().value("id");
    QString mediaPath=mediaHash.value(mediaId);
    QFileInfo fi(mediaPath);
    QString dir=fi.absolutePath(),name=fi.baseName();
    static QStringList supportedSubFormats={"","ass","ssa","srt"};
    genLog(QString("[%1]Request:Subtitle - %2").arg(socket->peerAddress().toString(),name));
    int formatIndex=0;
    for(int i=1;i<4;++i)
    {
        QFileInfo subInfo(dir,name+"."+supportedSubFormats[i]);
        if(subInfo.exists())
        {
            formatIndex=i;
            break;
        }
    }
    QJsonObject resposeObj
    {
        {"type", supportedSubFormats[formatIndex]}
    };
    QByteArray data = QJsonDocument(resposeObj).toJson();
    socket->setHeader("Content-Length", QByteArray::number(data.length()));
    socket->setHeader("Content-Type", "application/json");
    socket->writeHeaders();
    socket->write(data);
    socket->close();
}
