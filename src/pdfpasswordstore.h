// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QObject>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <memory>
#include <qqmlintegration.h>

struct PdfPasswordRequest;

// Only this service exchanges passwords with KWallet. Requests carry their own
// immutable document identity and can be cancelled without waiting for D-Bus.
class PdfPasswordStore : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY identityChanged)
    Q_PROPERTY(QString fileName READ fileName WRITE setFileName NOTIFY identityChanged)
    Q_PROPERTY(QString bookId READ bookId WRITE setBookId NOTIFY identityChanged)
    Q_PROPERTY(QWindow *window MEMBER m_window)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool passwordValid READ passwordValid NOTIFY passwordValidChanged)

public:
    explicit PdfPasswordStore(QObject *parent = nullptr);
    ~PdfPasswordStore() override;

    QUrl source() const;
    void setSource(const QUrl &source);
    QString fileName() const;
    void setFileName(const QString &fileName);
    QString bookId() const;
    void setBookId(const QString &bookId);
    bool busy() const;
    bool passwordValid() const;

    Q_INVOKABLE void load();
    Q_INVOKABLE void save(const QString &password);
    // Checks the saved credential against the original PDF; never exposes it to covers.
    Q_INVOKABLE void check(bool allowWalletOpen = true);
    Q_INVOKABLE void cancel();

Q_SIGNALS:
    void identityChanged();
    void busyChanged();
    void passwordValidChanged();
    void checked(bool valid);
    void invalidated(bool refresh);
    void loaded(const QString &password, bool found);
    void saved();
    void failed(const QString &message);

private:
    void start(bool write, const QString &password = {}, bool check = false, bool allowWalletOpen = true);
    void finish(quint64 id, const QString &key, const QString &password, bool found, const QString &error);
    void setPasswordValid(bool valid);
    void watchFile();
    void walletInvalidated(bool refresh);

    QUrl m_source;
    QString m_bookId;
    QString m_key;
    QWindow *m_window = nullptr;
    QTimer m_timeout;
    QFileSystemWatcher m_fileWatcher;
    QByteArray m_fileRevision;
    bool m_checking = false;
    bool m_passwordValid = false;
    std::shared_ptr<PdfPasswordRequest> m_request;
};
