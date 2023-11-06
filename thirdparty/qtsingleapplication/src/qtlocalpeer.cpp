/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the Qt Solutions component.
**
** $QT_BEGIN_LICENSE:BSD$
** You may use this file under the terms of the BSD license as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of Digia Plc and its Subsidiary(-ies) nor the names
**     of its contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/


#include "qtlocalpeer.h"
#include <QCoreApplication>
#include <QDataStream>


#if defined( Q_OS_WIN )
  #include <qt_windows.h>
#endif

#if defined(Q_OS_UNIX)
#include <ctime>
#include <unistd.h>
#endif

#include <QRegularExpression>


const char* QtLocalPeer::ack = "ack";

QtLocalPeer::QtLocalPeer(QObject* parent, const QString &appId)
    : QObject(parent), id(appId)
{
    if (id.isEmpty()) {
        id = QCoreApplication::applicationName();
    }
    QString prefix = id;
#if defined( Q_OS_WIN )
    prefix = prefix.toLower();
#endif

    prefix.remove(QRegularExpression("[^a-zA-Z]"));
    prefix.truncate(6);

    QByteArray idc = QDir::home().dirName().toUtf8();
#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )
    quint16 idNum = qChecksum( idc.constData(), idc.size() );
#else
    quint16 idNum = qChecksum( idc );
#endif

    socketName = QLatin1String("single-") + prefix
                 + QLatin1Char('-') + QString::number(idNum, 16);

    server = new QLocalServer(this);
    QString lockName = QDir(QDir::tempPath()).absolutePath()
                       + QLatin1Char('/') + socketName
                       + QLatin1String("-gdlockfile");
    lockFile = new QLockFile(lockName);
    lockFile->setStaleLockTime(0);
    if ( !lockFile->tryLock( 0 ) ) {
        if ( lockFile->error() != QLockFile::NoError && lockFile->error() != QLockFile::LockFailedError ) {
          qWarning() << "QLockFile error: " << lockFile->error();
        }
    }
}

bool QtLocalPeer::isClient()
{
    if ( !lockFile->isLocked() ) {
        return true;
    }

    bool res = server->listen(socketName);
#if defined(Q_OS_UNIX) && (QT_VERSION >= QT_VERSION_CHECK(4,5,0))
    // ### Workaround
    if (!res && server->serverError() == QAbstractSocket::AddressInUseError) {
        QFile::remove(QDir::cleanPath(QDir::tempPath())+QLatin1Char('/')+socketName);
        res = server->listen(socketName);
    }
#endif
    if (!res)
        qWarning("QtSingleCoreApplication: listen on local socket failed, %s", qPrintable(server->errorString()));
    QObject::connect( server, &QLocalServer::newConnection, this, &QtLocalPeer::receiveConnection );
    return false;
}

bool QtLocalPeer::sendMessage(const QString &message, int timeout)
{
    if (!isClient())
        return false;

    QLocalSocket socket;
    bool connOk = false;
    for(int i = 0; i < 2; i++) {
        // Try twice, in case the other instance is just starting up
        socket.connectToServer(socketName);
        connOk = socket.waitForConnected(timeout/2);
        if (connOk || i)
            break;
        int ms = 250;
#if defined(Q_OS_WIN)
        Sleep(DWORD(ms));
#else
        struct timespec ts = { ms / 1000, (ms % 1000) * 1000 * 1000 };
        nanosleep(&ts, NULL);
#endif
    }
    if (!connOk)
        return false;

    QByteArray uMsg(message.toUtf8());
    QDataStream ds(&socket);
    ds.writeBytes(uMsg.constData(), uMsg.size());
    bool res = socket.waitForBytesWritten(timeout);
    if (res) {
        res &= socket.waitForReadyRead(timeout);   // wait for ack
        if (res)
            res &= (socket.read(qstrlen(ack)) == ack);
    }
    return res;
}

void QtLocalPeer::receiveConnection()
{
    QLocalSocket* socket = server->nextPendingConnection();
    if (!socket)
        return;

    while (socket->bytesAvailable() < (int)sizeof(quint32))
        socket->waitForReadyRead();
    QDataStream ds(socket);
    QByteArray uMsg;
    quint32 remaining;
    ds >> remaining;
    uMsg.resize(remaining);
    int got = 0;
    char* uMsgBuf = uMsg.data();
    do {
        got = ds.readRawData(uMsgBuf, remaining);
        remaining -= got;
        uMsgBuf += got;
    } while (remaining && got >= 0 && socket->waitForReadyRead(2000));
    if (got < 0) {
        qWarning("QtLocalPeer: Message reception failed %s", socket->errorString().toLatin1().constData());
        delete socket;
        return;
    }
    QString message(QString::fromUtf8(uMsg));
    socket->write(ack, qstrlen(ack));
    socket->waitForBytesWritten(1000);
    delete socket;
    emit messageReceived(message); //### (might take a long time to return)
}

QtLocalPeer::~QtLocalPeer() {
    server->close();
    lockFile->unlock(); // Ensure file unlocked
}
