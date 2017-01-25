#include "urlintercepter.h"
#include <iostream>
#include <QWebEngineUrlRequestInterceptor>



void RequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    if (!info.requestUrl().host().contains("qt")) {
        qDebug() << "blocking" << info.requestUrl();
        //info.block(true);
    }
}
