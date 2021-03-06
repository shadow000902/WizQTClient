#include "wizNoteManager.h"
#include <QFileInfo>
#include <QSettings>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDir>
#include <fstream>
#include <QDebug>

#include "rapidjson/document.h"
#include "utils/pathresolve.h"
#include "utils/misc.h"
#include "sync/apientry.h"
#include "sync/token.h"
#include "share/wizobject.h"
#include "share/wizDatabase.h"
#include "share/wizDatabaseManager.h"
#include "share/wizthreads.h"
#include "share/wizEventLoop.h"
#include "wizDocTemplateDialog.h"
#include "share/wizObjectDataDownloader.h"

void CWizNoteManager::createIntroductionNoteForNewRegisterAccount()
{
    WizExecuteOnThread(WIZ_THREAD_DEFAULT, [=](){
        //get local note
        QDir dir(Utils::PathResolve::introductionNotePath());
        QStringList introductions = dir.entryList(QDir::Files);
        if (introductions.isEmpty())
            return;

        QSettings settings(Utils::PathResolve::introductionNotePath() + "settings.ini", QSettings::IniFormat);
        //copy note to new account
        CWizDatabase& db = m_dbMgr.db();
        for (QString fileName : introductions)
        {
            QString filePath = Utils::PathResolve::introductionNotePath() + fileName;
            QFileInfo info(filePath);
            if (info.suffix() == "ini")
                continue;
            settings.beginGroup("Location");
            QString location = settings.value(info.baseName(), "/My Notes/").toByteArray();
            settings.endGroup();
            WIZTAGDATA tag;
            WIZDOCUMENTDATA doc;
            settings.beginGroup("Title");
            doc.strTitle = settings.value(info.baseName()).toString();
            settings.endGroup();
            if (!db.CreateDocumentByTemplate(filePath, location, tag, doc))
            {
                qCritical() << "create introduction note failed : " << filePath;
            }
        }
    });
}

bool CWizNoteManager::createNote(WIZDOCUMENTDATA& data)
{
    return createNote(data, "", QObject::tr("Untitled"), "<p><br/></p>", "");
}

bool CWizNoteManager::createNote(WIZDOCUMENTDATA& data, const QString& strKbGUID)
{
    return createNote(data, strKbGUID, QObject::tr("Untitled"), "<p><br/></p>", "");
}

bool CWizNoteManager::createNote(WIZDOCUMENTDATA& data, const QString& strKbGUID,
                                 const QString& strLocation)
{
    return createNote(data, strKbGUID, QObject::tr("Untitled"), "<p><br/></p>", strLocation);
}

bool CWizNoteManager::createNote(WIZDOCUMENTDATA& data, const QString& strKbGUID,
                                 const WIZTAGDATA& tag)
{
    return createNote(data, strKbGUID, QObject::tr("Untitled"), "<p><br/></p>", "", tag);
}

bool CWizNoteManager::createNote(WIZDOCUMENTDATA& data, const QString& strKbGUID,
                                 const QString& strLocation, const WIZTAGDATA& tag)
{
    return createNote(data, strKbGUID, QObject::tr("Untitled"), "<p><br/></p>", strLocation, tag);
}

bool CWizNoteManager::createNote(WIZDOCUMENTDATA& data, const QString& strKbGUID,
                                 const QString& strTitle, const QString& strHtml)
{
    return createNote(data, strKbGUID, strTitle, strHtml, "");
}

bool CWizNoteManager::createNote(WIZDOCUMENTDATA& data, const QString& strKbGUID,
                                 const QString& strTitle, const QString& strHtml,
                                 const QString& strLocation)
{
    QString location = strLocation;
    if (location.isEmpty())
    {
        location = m_dbMgr.db(strKbGUID).GetDefaultNoteLocation();
    }

    if (data.strType.isEmpty())
    {
        data.strType = WIZ_DOCUMENT_TYPE_NORMAL;
    }

    if (!m_dbMgr.db(strKbGUID).CreateDocumentAndInit(strHtml, "", 0, strTitle, "newnote", location, "", data))
    {
        qCritical() << "Failed to new document!";
        return false;
    }

    return true;
}


bool CWizNoteManager::createNote(WIZDOCUMENTDATA& data, const QString& strKbGUID,
                                 const QString& strTitle, const QString& strHtml,
                                 const WIZTAGDATA& tag)
{
    QString location = m_dbMgr.db(strKbGUID).GetDefaultNoteLocation();
    return createNote(data, strKbGUID, strTitle, strHtml, location, tag);
}

bool CWizNoteManager::createNote(WIZDOCUMENTDATA& data, const QString& strKbGUID,
                                 const QString& strTitle, const QString& strHtml,
                                 const QString& strLocation, const WIZTAGDATA& tag)
{
    if (!createNote(data, strKbGUID, strTitle, strHtml, strLocation))
        return false;

    if (!tag.strGUID.IsEmpty())
    {
        CWizDocument doc(m_dbMgr.db(strKbGUID), data);
        doc.AddTag(tag);
    }

    return true;
}

bool CWizNoteManager::createNoteByTemplate(WIZDOCUMENTDATA& data, const WIZTAGDATA& tag, const QString& strZiw)
{
    //通过模板创建笔记时，如果模板文件不存在则创建一篇空笔记
    if (!QFile::exists(strZiw))
    {
        qDebug() << "Template file not exists : " << strZiw;
        return createNote(data, data.strKbGUID, data.strTitle, "<p><br></p>", data.strLocation, tag);
    }

    if (!m_dbMgr.db(data.strKbGUID).CreateDocumentByTemplate(strZiw, data.strLocation, tag, data))
    {
        qDebug() << "Failed to new document! " << strZiw;
        return false;
    }
    return true;
}

void CWizNoteManager::updateTemplateJS(const QString& local)
{
    //软件启动之后获取模板信息，检查template.js是否存在、是否是最新版。需要下载时进行下载
    WizExecuteOnThread(WIZ_THREAD_NETWORK, [=]() {
        //NOTE:现在编辑器依赖template.js文件。需要确保该文件存在。如果文件不存在则拷贝
        WizEnsurePathExists(Utils::PathResolve::customNoteTemplatesPath());
        if (!QFile::exists(Utils::PathResolve::wizTemplateJsFilePath()))
        {
            QString localJs = Utils::PathResolve::resourcesPath() + "files/wizeditor/wiz_template.js";
            WizCopyFile(localJs, Utils::PathResolve::wizTemplateJsFilePath(), true);
        }

        QNetworkAccessManager manager;
        QString url = CommonApiEntry::asServerUrl() + "/a/templates?language_type=" + local;
#ifdef Q_OS_MAC
        url.append("&client_type=macosx");
#else
        url.append("&client_type=linux");
#endif
//        qDebug() << "get templates message from url : " << url;
        //
        QByteArray ba;
        {
            QNetworkReply* reply = manager.get(QNetworkRequest(url));
            CWizAutoTimeOutEventLoop loop(reply);
            loop.exec();
            //
            if (loop.error() != QNetworkReply::NoError || loop.result().isEmpty())
                return;

            ba = loop.result();
        }

        //根据线上的内容来判断本地的模板文件是否需要更新
        if (!updateLocalTemplates(ba, manager))
            return;

        //更新成功之后将数据保存到本地
        QString jsonFile = Utils::PathResolve::wizTemplateJsonFilePath();
        std::ofstream logFile(jsonFile.toUtf8().constData(), std::ios::out | std::ios::trunc);
        logFile << ba.constData();
    });
}

void CWizNoteManager::downloadTemplatePurchaseRecord()
{
    //下载用户购买的模板列表
    WizExecuteOnThread(WIZ_THREAD_NETWORK, [=]() {
        WizEnsurePathExists(Utils::PathResolve::customNoteTemplatesPath());
        //
        QNetworkAccessManager manager;
        QString url = CommonApiEntry::asServerUrl() + "/a/templates/record?token=" + Token::token();
//        qDebug() << "get templates record from url : " << url;
        //
        QByteArray ba;
        {
            QNetworkReply* reply = manager.get(QNetworkRequest(url));
            CWizAutoTimeOutEventLoop loop(reply);
            loop.exec();
            //
            if (loop.error() != QNetworkReply::NoError || loop.result().isEmpty())
                return;

            ba = loop.result();
            QString jsonFile = Utils::PathResolve::wizTemplatePurchaseRecordFile();
            std::ofstream recordFile(jsonFile.toUtf8().constData(), std::ios::out | std::ios::trunc);
            recordFile << ba.constData();
        }
    });
}

bool CWizNoteManager::updateLocalTemplates(const QByteArray& newJsonData, QNetworkAccessManager& manager)
{
    rapidjson::Document d;
    d.Parse(newJsonData.constData());
    if (d.HasParseError())
        return false;

    QString localFile = Utils::PathResolve::wizTemplateJsonFilePath();
    bool needUpdateJs = true;
    QMap<int, TemplateData> localTmplMap;
    QFile file(localFile);
    if (file.open(QFile::ReadOnly))
    {
        QTextStream stream(&file);
        QString jsonData = stream.readAll();
        rapidjson::Document localD;
        localD.Parse(jsonData.toUtf8().constData());

        if (!localD.HasParseError())
        {
            if (localD.HasMember("template_js_version") && d.HasMember("template_js_version"))
            {
                needUpdateJs = (localD.FindMember("template_js_version")->value.GetString() !=
                        d.FindMember("template_js_version")->value.GetString());
            }
        }

        getTemplatesFromJsonData(jsonData.toUtf8(), localTmplMap);
    }

    //
    if (needUpdateJs)
    {
        QString link;
        if (d.HasMember("template_js_link"))
        {
            link = d.FindMember("template_js_link")->value.GetString();
        }
        if (!link.isEmpty())
        {
            qDebug() << "get templates js file from url : " << link;
            QString file = Utils::PathResolve::wizTemplateJsFilePath();
            QNetworkReply* reply = manager.get(QNetworkRequest(link));
            //
            CWizAutoTimeOutEventLoop loop(reply);
            loop.exec();
            //
            if (loop.error() != QNetworkReply::NoError || loop.result().isEmpty())
                return false;

            QByteArray ba = loop.result();
            std::ofstream jsFile(file.toUtf8().constData(), std::ios::out | std::ios::trunc);
            jsFile << ba.constData();
        }
    }

    //
    QMap<int, TemplateData> serverTmplMap;
    getTemplatesFromJsonData(newJsonData, serverTmplMap);

    //下载服务器上有更新的模板
    for (auto it = serverTmplMap.begin(); it != serverTmplMap.end(); ++it)
    {
        auto iter = localTmplMap.find(it.key());
        if (iter == localTmplMap.end())
            continue;

        if (iter.value().strVersion != it.value().strVersion || !QFile::exists(it.value().strFileName))
        {
            QString strUrl = CommonApiEntry::asServerUrl() + "/a/templates/download/" + QString::number(it.value().id);
            QFileInfo info(it.value().strFileName);
            CWizFileDownloader* downloader = new CWizFileDownloader(strUrl, info.fileName(), info.absolutePath() + "/", false);
            downloader->startDownload();
        }
    }

    //删除服务器上不存在的模板
    for (auto it = localTmplMap.begin(); it != localTmplMap.end(); ++it)
    {
        auto iter = serverTmplMap.find(it.key());
        if (iter == localTmplMap.end())
        {
            WizDeleteFile(it.value().strFileName);
        }
    }

    return true;
}

bool CWizNoteManager::downloadTemplateBlocked(const TemplateData& tempData)
{
    QString strUrl = CommonApiEntry::asServerUrl() + "/a/templates/download/" + QString::number(tempData.id);
    return WizURLDownloadToFile(strUrl, tempData.strFileName, false);
}


CWizNoteManager::CWizNoteManager(CWizDatabaseManager& dbMgr)
    : m_dbMgr(dbMgr)
{
}

