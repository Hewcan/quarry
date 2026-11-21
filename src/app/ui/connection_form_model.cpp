#include "connection_form_model.h"

#include <algorithm>

namespace quarry::ui {

using Kind = quarry::ConnectionField::Kind;

ConnectionFormModel::ConnectionFormModel(QObject *parent)
    : QAbstractListModel(parent)
{ }

QString ConnectionFormModel::kindName(Kind kind)
{
    switch (kind) {
    case Kind::Number:
        return QStringLiteral("number");
    case Kind::Path:
        return QStringLiteral("path");
    case Kind::Enum:
        return QStringLiteral("enum");
    case Kind::Bool:
        return QStringLiteral("bool");
    case Kind::Text:
    case Kind::Secret:
        break;
    }
    return QStringLiteral("text");
}

void ConnectionFormModel::setFields(const std::vector<quarry::ConnectionField> &fields,
                                    bool sshAvailable)
{
    beginResetModel();
    m_rows.clear();
    m_secretKeys.clear();
    for (const auto &f : fields) {
        if (f.kind == Kind::Secret) {
            m_secretKeys.append(f.key);
            continue;
        }
        Row row;
        row.field = f;
        if (f.kind == Kind::Bool)
            row.value = f.defaultValue.toBool();
        else if (f.kind == Kind::Enum)
            row.value = f.defaultValue.isValid()
                ? f.defaultValue.toString()
                : (f.enumValues.isEmpty() ? QString() : f.enumValues.constFirst());
        else
            row.value = f.defaultValue.toString();
        m_rows.push_back(std::move(row));
    }
    m_ssh = {};
    m_sshAvailable = sshAvailable;
    endResetModel();
    emit fieldsChanged();
    emit sshChanged();
    emit validChanged();
}

void ConnectionFormModel::setValues(const QVariantMap &values)
{
    for (auto &row : m_rows) {
        const auto it = values.constFind(row.field.key);
        if (it == values.constEnd())
            continue;
        row.value
            = row.field.kind == Kind::Bool ? QVariant(it->toBool()) : QVariant(it->toString());
    }
    if (!m_rows.empty())
        emit dataChanged(index(0), index(static_cast<int>(m_rows.size()) - 1));
    if (m_sshAvailable) {
        const SshTunnelConfig config = SshTunnelConfig::fromValues(values);
        m_ssh = {};
        m_ssh.enabled = config.enabled;
        if (config.enabled) {
            m_ssh.host = config.host;
            m_ssh.port = QString::number(config.port);
            m_ssh.user = config.user;
            m_ssh.auth = config.auth == SshAuth::Key ? QStringLiteral("key")
                : config.auth == SshAuth::Agent      ? QStringLiteral("agent")
                                                     : QStringLiteral("password");
            m_ssh.keyPath = config.keyPath;
            m_ssh.secretMode
                = values.value(QLatin1String(kSshSecretModeKey)).toString() == QLatin1String("ask")
                ? 2
                : 1;
        }
        emit sshChanged();
    }
    emit validChanged();
}

QVariantMap ConnectionFormModel::values() const
{
    QVariantMap out;
    for (const auto &row : m_rows) {
        const QString &key = row.field.key;
        switch (row.field.kind) {
        case Kind::Bool:
            out.insert(key, row.value.toBool());
            break;
        case Kind::Number: {
            const QString text = row.value.toString().trimmed();
            bool ok = false;
            const qlonglong n = text.toLongLong(&ok);
            if (ok)
                out.insert(key, n);
            break;
        }
        default: {
            const QString text = row.value.toString();
            if (!text.isEmpty())
                out.insert(key, text);
            break;
        }
        }
    }
    if (m_sshAvailable) {
        sshConfig().toValues(out);
        if (sshNeedsSecret())
            out.insert(QLatin1String(kSshSecretModeKey),
                       m_ssh.secretMode == 2 ? QStringLiteral("ask") : QStringLiteral("keychain"));
    }
    return out;
}

bool ConnectionFormModel::databaseBlank() const
{
    return std::ranges::any_of(m_rows, [](const Row &row) {
        return row.field.key == QLatin1String("database")
            && row.value.toString().trimmed().isEmpty();
    });
}

bool ConnectionFormModel::hasAdvanced() const
{
    for (const auto &row : m_rows)
        if (row.field.advanced)
            return true;
    return false;
}

void ConnectionFormModel::setValue(int row, const QVariant &value)
{
    if (row < 0 || row >= rowCount())
        return;
    auto &r = m_rows[static_cast<size_t>(row)];
    r.value = value;
    emit dataChanged(index(row), index(row), { ValueRole, ErrorRole });
    emit validChanged();
}

QString ConnectionFormModel::errorFor(const Row &row) const
{
    const QString text = row.value.toString().trimmed();
    if (row.field.kind == Kind::Number && !text.isEmpty()) {
        bool ok = false;
        text.toLongLong(&ok);
        if (!ok)
            return tr("Enter a whole number.");
    }
    const bool empty = row.field.kind != Kind::Bool && text.isEmpty();
    if (row.field.required && empty)
        return tr("This field is required.");
    return {};
}

bool ConnectionFormModel::isValid() const
{
    return std::ranges::all_of(m_rows, [this](const Row &r) { return errorFor(r).isEmpty(); })
        && (!sshEnabled() || (sshPortValid() && sshConfig().validationProblem().isEmpty()));
}

// ---- SSH tunnel --------------------------------------------------------------------------------

bool ConnectionFormModel::sshPortValid() const
{
    bool ok = false;
    const int port = m_ssh.port.trimmed().toInt(&ok);
    return ok && port >= 1 && port <= 65535;
}

SshTunnelConfig ConnectionFormModel::sshConfig() const
{
    QVariantMap v;
    v.insert(QLatin1String(SshTunnelConfig::kEnabledKey), sshEnabled());
    v.insert(QLatin1String(SshTunnelConfig::kHostKey), m_ssh.host);
    v.insert(QLatin1String(SshTunnelConfig::kPortKey), m_ssh.port.trimmed());
    v.insert(QLatin1String(SshTunnelConfig::kUserKey), m_ssh.user);
    v.insert(QLatin1String(SshTunnelConfig::kAuthKey), m_ssh.auth);
    v.insert(QLatin1String(SshTunnelConfig::kKeyPathKey), m_ssh.keyPath);
    return SshTunnelConfig::fromValues(v);
}

void ConnectionFormModel::sshTouched()
{
    emit sshChanged();
    emit validChanged();
}

void ConnectionFormModel::setSshEnabled(bool enabled)
{
    if (enabled == m_ssh.enabled)
        return;
    m_ssh.enabled = enabled;
    sshTouched();
}

void ConnectionFormModel::setSshHost(const QString &host)
{
    if (host == m_ssh.host)
        return;
    m_ssh.host = host;
    sshTouched();
}

void ConnectionFormModel::setSshPort(const QString &port)
{
    if (port == m_ssh.port)
        return;
    m_ssh.port = port;
    sshTouched();
}

void ConnectionFormModel::setSshUser(const QString &user)
{
    if (user == m_ssh.user)
        return;
    m_ssh.user = user;
    sshTouched();
}

void ConnectionFormModel::setSshAuth(const QString &auth)
{
    const QString value = auth == QLatin1String("key") || auth == QLatin1String("agent")
        ? auth
        : QStringLiteral("password");
    if (value == m_ssh.auth)
        return;
    m_ssh.auth = value;
    sshTouched();
}

void ConnectionFormModel::setSshKeyPath(const QString &path)
{
    if (path == m_ssh.keyPath)
        return;
    m_ssh.keyPath = path;
    sshTouched();
}

void ConnectionFormModel::setSshSecretMode(int mode)
{
    const int value = mode == 2 ? 2 : 1;
    if (value == m_ssh.secretMode)
        return;
    m_ssh.secretMode = value;
    sshTouched();
}

QString ConnectionFormModel::sshHostError() const
{
    return sshEnabled() && m_ssh.host.trimmed().isEmpty() ? tr("Enter the SSH server host.")
                                                          : QString();
}

QString ConnectionFormModel::sshPortError() const
{
    return sshEnabled() && !sshPortValid() ? tr("The SSH port must be between 1 and 65535.")
                                           : QString();
}

QString ConnectionFormModel::sshUserError() const
{
    return sshEnabled() && m_ssh.user.trimmed().isEmpty() ? tr("Enter the SSH user name.")
                                                          : QString();
}

QString ConnectionFormModel::sshKeyPathError() const
{
    return sshEnabled() && m_ssh.auth == QLatin1String("key") && m_ssh.keyPath.trimmed().isEmpty()
        ? tr("Choose the SSH private key file.")
        : QString();
}

int ConnectionFormModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant ConnectionFormModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const Row &row = m_rows[static_cast<size_t>(index.row())];
    switch (role) {
    case KeyRole:
        return row.field.key;
    case LabelRole:
        return row.field.label;
    case KindRole:
        return kindName(row.field.kind);
    case RequiredRole:
        return row.field.required;
    case AdvancedRole:
        return row.field.advanced;
    case EnumValuesRole:
        return row.field.enumValues;
    case ValueRole:
        return row.value;
    case ErrorRole:
        return errorFor(row);
    default:
        return {};
    }
}

QHash<int, QByteArray> ConnectionFormModel::roleNames() const
{
    return {
        { KeyRole, "key" },
        { LabelRole, "label" },
        { KindRole, "kind" },
        { RequiredRole, "isRequired" },
        { AdvancedRole, "isAdvanced" },
        { EnumValuesRole, "enumValues" },
        { ValueRole, "value" },
        { ErrorRole, "error" },
    };
}

} // namespace quarry::ui
