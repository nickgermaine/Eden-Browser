#pragma once

#include <QAbstractListModel>
#include <QVariantList>

namespace eden::core {

class ThemeManager;

class ThemeEditorModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString selectedSection READ selectedSection WRITE setSelectedSection NOTIFY selectedSectionChanged)
    Q_PROPERTY(QVariantList navigationSections READ navigationSections CONSTANT)

  public:
    enum Role {
        PathRole = Qt::UserRole + 1,
        LabelRole,
        DescriptionRole,
        SectionRole,
        KindRole,
        ValueRole,
        MinimumRole,
        MaximumRole,
        UnitRole,
        OptionsRole,
        SectionStartRole,
        PairedRole,
        LightPathRole,
        DarkPathRole,
        LightValueRole,
        DarkValueRole
    };

    struct TokenDefinition {
        QString path;
        QString label;
        QString description;
        QString section;
        QString kind;
        QString unit;
        QString lightPath;
        QString darkPath;
        qreal minimum = 0;
        qreal maximum = 0;
        QStringList options;
    };

    struct SectionDefinition {
        QString title;
        QString description;
    };

    explicit ThemeEditorModel(ThemeManager *manager);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    const TokenDefinition *definition(const QString &path) const;
    void refreshValues();

    QString selectedSection() const;
    void setSelectedSection(const QString &section);
    QVariantList navigationSections() const;
    Q_INVOKABLE int firstIndexForSection(const QString &section) const;
    Q_INVOKABLE QString sectionDescription(const QString &section) const;

  signals:
    void selectedSectionChanged();

  private:
    void addSection(const QString &title, const QString &description);
    void addBaseColors();
    void addSchemeColors();
    void addToken(const QString &path, const QString &label, const QString &description, const QString &section, const QString &kind,
                  qreal minimum = 0, qreal maximum = 0, const QString &unit = {}, const QStringList &options = {});
    void addSchemeToken(const QString &path, const QString &label, const QString &description, const QString &section, const QString &kind,
                        qreal minimum = 0, qreal maximum = 0, const QString &unit = {}, const QStringList &options = {});
    void rebuildVisibleRows();

    ThemeManager *m_manager;
    QList<TokenDefinition> m_tokens;
    QList<SectionDefinition> m_sections;
    QList<int> m_visibleRows;
    QString m_selectedSection = "Overview";
};

}
