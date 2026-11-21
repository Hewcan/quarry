#pragma once

#include <quarry/provider_api.h>
#include <quarry/ssh_tunnel_config.h>

#include <QAbstractListModel>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <vector>

namespace quarry::ui {

// Where the "save in the keychain / ask every time" choice of the SSH secret lives in
// ConnectionProfile::values: "keychain" (default) or "ask". The tunnel ignores it; it is not a
// secret.
inline constexpr const char *kSshSecretModeKey = "ssh.secretMode";

// The fields of one engine's connection form (ConnectionField list of the provider), edited by
// the dialog. Secret fields are not rows: they are the single password control of the dialog and
// only their keys are kept (secretKeys()), to hand to ConnectionStore::save(). The SSH tunnel
// section is not made of rows either: it is a fixed set of properties, shown only when the engine
// has Capability::Ssh, and stored in values() under the SshTunnelConfig keys.
class ConnectionFormModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by ConnectionController")

    Q_PROPERTY(bool hasAdvanced READ hasAdvanced NOTIFY fieldsChanged FINAL)
    Q_PROPERTY(bool hasSecret READ hasSecret NOTIFY fieldsChanged FINAL)
    // No row has an error. Rows are checked live: the dialog decides when to show the message.
    Q_PROPERTY(bool valid READ isValid NOTIFY validChanged FINAL)
    // The engine has a "database" field and it is blank: the connection spans the server's
    // databases (the dialog then leaves the migrations of each database to the tree).
    Q_PROPERTY(bool databaseBlank READ databaseBlank NOTIFY validChanged FINAL)

    // SSH tunnel section. sshAuth: "password" | "key" | "agent". sshSecretMode: 1 keychain,
    // 2 ask every time. The *Error properties are empty while the tunnel is off or the field is
    // fine; they are computed live and the dialog decides when to show them.
    Q_PROPERTY(bool sshAvailable READ sshAvailable NOTIFY sshChanged FINAL)
    Q_PROPERTY(bool sshEnabled READ sshEnabled WRITE setSshEnabled NOTIFY sshChanged FINAL)
    Q_PROPERTY(QString sshHost READ sshHost WRITE setSshHost NOTIFY sshChanged FINAL)
    Q_PROPERTY(QString sshPort READ sshPort WRITE setSshPort NOTIFY sshChanged FINAL)
    Q_PROPERTY(QString sshUser READ sshUser WRITE setSshUser NOTIFY sshChanged FINAL)
    Q_PROPERTY(QString sshAuth READ sshAuth WRITE setSshAuth NOTIFY sshChanged FINAL)
    Q_PROPERTY(QString sshKeyPath READ sshKeyPath WRITE setSshKeyPath NOTIFY sshChanged FINAL)
    Q_PROPERTY(int sshSecretMode READ sshSecretMode WRITE setSshSecretMode NOTIFY sshChanged FINAL)
    Q_PROPERTY(bool sshNeedsSecret READ sshNeedsSecret NOTIFY sshChanged FINAL)
    Q_PROPERTY(QString sshHostError READ sshHostError NOTIFY sshChanged FINAL)
    Q_PROPERTY(QString sshPortError READ sshPortError NOTIFY sshChanged FINAL)
    Q_PROPERTY(QString sshUserError READ sshUserError NOTIFY sshChanged FINAL)
    Q_PROPERTY(QString sshKeyPathError READ sshKeyPathError NOTIFY sshChanged FINAL)

public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        LabelRole,
        KindRole, // "text" | "number" | "path" | "enum" | "bool"
        RequiredRole,
        AdvancedRole,
        EnumValuesRole,
        ValueRole, // number fields are edited as text
        ErrorRole, // plain message, empty when valid (computed from the value)
    };

    explicit ConnectionFormModel(QObject *parent = nullptr);

    // `sshAvailable`: the engine has Capability::Ssh. The tunnel draft goes back to its defaults.
    void setFields(const std::vector<quarry::ConnectionField> &fields, bool sshAvailable = false);
    // Fills the rows from saved values; keys the engine does not know are ignored.
    void setValues(const QVariantMap &values);
    // Typed values to store. Empty optional text/number fields are left out so defaults apply.
    QVariantMap values() const;
    QStringList secretKeys() const { return m_secretKeys; }
    bool hasSecret() const { return !m_secretKeys.isEmpty(); }
    bool hasAdvanced() const;

    bool sshAvailable() const { return m_sshAvailable; }
    bool sshEnabled() const { return m_sshAvailable && m_ssh.enabled; }
    QString sshHost() const { return m_ssh.host; }
    QString sshPort() const { return m_ssh.port; }
    QString sshUser() const { return m_ssh.user; }
    QString sshAuth() const { return m_ssh.auth; }
    QString sshKeyPath() const { return m_ssh.keyPath; }
    int sshSecretMode() const { return m_ssh.secretMode; }
    bool sshNeedsSecret() const { return sshEnabled() && m_ssh.auth != QLatin1String("agent"); }
    void setSshEnabled(bool enabled);
    void setSshHost(const QString &host);
    void setSshPort(const QString &port);
    void setSshUser(const QString &user);
    void setSshAuth(const QString &auth);
    void setSshKeyPath(const QString &path);
    void setSshSecretMode(int mode);
    QString sshHostError() const;
    QString sshPortError() const;
    QString sshUserError() const;
    QString sshKeyPathError() const;
    // The tunnel settings as the core sees them (what validationProblem() checks).
    quarry::SshTunnelConfig sshConfig() const;

    Q_INVOKABLE void setValue(int row, const QVariant &value);
    bool isValid() const;
    bool databaseBlank() const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void fieldsChanged();
    void validChanged();
    void sshChanged();

private:
    struct Row
    {
        quarry::ConnectionField field;
        QVariant value;
    };

    struct SshDraft
    {
        bool enabled = false;
        QString host;
        QString port = QStringLiteral("22");
        QString user;
        QString auth = QStringLiteral("password");
        QString keyPath;
        int secretMode = 1;
    };

    bool sshPortValid() const;
    void sshTouched();
    static QString kindName(quarry::ConnectionField::Kind kind);
    QString errorFor(const Row &row) const;

    std::vector<Row> m_rows;
    QStringList m_secretKeys;
    SshDraft m_ssh;
    bool m_sshAvailable = false;
};

} // namespace quarry::ui
