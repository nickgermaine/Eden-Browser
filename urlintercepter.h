#ifndef URLINTERCEPTER_H
#define URLINTERCEPTER_H

#include <QObject>
#include <QWebEngineUrlRequestInterceptor>
#include <QtWebEngineWidgets>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInfo>
#include <QtWebEngineCore/qwebengineurlrequestinterceptor.h>
#include <QDebug>


class RequestInterceptor : public QWebEngineUrlRequestInterceptor
{
    Q_OBJECT
public:
    RequestInterceptor(QObject *parent = nullptr) : QWebEngineUrlRequestInterceptor(parent)
    {
    }

    void interceptRequest(QWebEngineUrlRequestInfo &info);
};

#endif // URLINTERCEPTER_H
