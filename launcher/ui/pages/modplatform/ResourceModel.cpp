// SPDX-FileCopyrightText: 2023 flowln <flowlnlnln@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-only

#include "ResourceModel.h"

#include <QCryptographicHash>
#include <QIcon>
#include <QMessageBox>
#include <QPixmapCache>
#include <QUrl>

#include "Application.h"
#include "BuildConfig.h"
#include "Json.h"

#include "net/Download.h"
#include "net/NetJob.h"

#include "modplatform/ModIndex.h"

#include "ui/widgets/ProjectItem.h"

namespace ResourceDownload {

QHash<ResourceModel*, bool> ResourceModel::s_running_models;

ResourceModel::ResourceModel(ResourceAPI* api) : QAbstractListModel(), m_api(api)
{
    s_running_models.insert(this, true);
}

ResourceModel::~ResourceModel()
{
    s_running_models.find(this).value() = false;
}

auto ResourceModel::data(const QModelIndex& index, int role) const -> QVariant
{
    int pos = index.row();
    if (pos >= m_packs.size() || pos < 0 || !index.isValid()) {
        return QString("INVALID INDEX %1").arg(pos);
    }

    auto pack = m_packs.at(pos);
    switch (role) {
        case Qt::ToolTipRole: {
            if (pack.description.length() > 100) {
                // some magic to prevent to long tooltips and replace html linebreaks
                QString edit = pack.description.left(97);
                edit = edit.left(edit.lastIndexOf("<br>")).left(edit.lastIndexOf(" ")).append("...");
                return edit;
            }
            return pack.description;
        }
        case Qt::DecorationRole: {
            if (auto icon_or_none = const_cast<ResourceModel*>(this)->getIcon(const_cast<QModelIndex&>(index), pack.logoUrl);
                icon_or_none.has_value())
                return icon_or_none.value();

            return APPLICATION->getThemedIcon("screenshot-placeholder");
        }
        case Qt::SizeHintRole:
            return QSize(0, 58);
        case Qt::UserRole: {
            QVariant v;
            v.setValue(pack);
            return v;
        }
            // Custom data
        case UserDataTypes::TITLE:
            return pack.name;
        case UserDataTypes::DESCRIPTION:
            return pack.description;
        case UserDataTypes::SELECTED:
            return pack.isAnyVersionSelected();
        default:
            break;
    }

    return {};
}

QHash<int, QByteArray> ResourceModel::roleNames() const
{
    QHash<int, QByteArray> roles;

    roles[Qt::ToolTipRole] = "toolTip";
    roles[Qt::DecorationRole] = "decoration";
    roles[Qt::SizeHintRole] = "sizeHint";
    roles[Qt::UserRole] = "pack";
    roles[UserDataTypes::TITLE] = "title";
    roles[UserDataTypes::DESCRIPTION] = "description";
    roles[UserDataTypes::SELECTED] = "selected";

    return roles;
}

bool ResourceModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    int pos = index.row();
    if (pos >= m_packs.size() || pos < 0 || !index.isValid())
        return false;

    m_packs[pos] = value.value<ModPlatform::IndexedPack>();
    emit dataChanged(index, index);

    return true;
}

QString ResourceModel::debugName() const
{
    return "ResourceDownload (Model)";
}

void ResourceModel::fetchMore(const QModelIndex& parent)
{
    if (parent.isValid() || m_search_state == SearchState::Finished)
        return;

    search();
}

void ResourceModel::search()
{
    if (hasActiveSearchJob())
        return;

    auto args{ createSearchArguments() };

    auto callbacks{ createSearchCallbacks() };

    // Use defaults if no callbacks are set
    if (!callbacks.on_succeed)
        callbacks.on_succeed = [this](auto& doc) {
            if (!s_running_models.constFind(this).value())
                return;
            searchRequestSucceeded(doc);
        };
    if (!callbacks.on_fail)
        callbacks.on_fail = [this](QString reason, int network_error_code) {
            if (!s_running_models.constFind(this).value())
                return;
            searchRequestFailed(reason, network_error_code);
        };
    if (!callbacks.on_abort)
        callbacks.on_abort = [this] {
            if (!s_running_models.constFind(this).value())
                return;
            searchRequestAborted();
        };

    if (auto job = m_api->searchProjects(std::move(args), std::move(callbacks)); job)
        runSearchJob(job);
}

void ResourceModel::loadEntry(QModelIndex& entry)
{
    auto const& pack = m_packs[entry.row()];

    if (!hasActiveInfoJob())
        m_current_info_job.clear();

    if (!pack.versionsLoaded) {
        auto args{ createVersionsArguments(entry) };
        auto callbacks{ createVersionsCallbacks(entry) };

        // Use default if no callbacks are set
        if (!callbacks.on_succeed)
            callbacks.on_succeed = [this, entry](auto& doc, auto pack) {
                if (!s_running_models.constFind(this).value())
                    return;
                versionRequestSucceeded(doc, pack, entry);
            };

        if (auto job = m_api->getProjectVersions(std::move(args), std::move(callbacks)); job)
            runInfoJob(job);
    }

    if (!pack.extraDataLoaded) {
        auto args{ createInfoArguments(entry) };
        auto callbacks{ createInfoCallbacks(entry) };

        // Use default if no callbacks are set
        if (!callbacks.on_succeed)
            callbacks.on_succeed = [this, entry](auto& doc, auto pack) {
                if (!s_running_models.constFind(this).value())
                    return;
                infoRequestSucceeded(doc, pack, entry);
            };

        if (auto job = m_api->getProjectInfo(std::move(args), std::move(callbacks)); job)
            runInfoJob(job);
    }
}

void ResourceModel::refresh()
{
    bool reset_requested = false;

    if (hasActiveInfoJob()) {
        m_current_info_job.abort();
        reset_requested = true;
    }

    if (hasActiveSearchJob()) {
        m_current_search_job->abort();
        reset_requested = true;
    }

    if (reset_requested) {
        m_search_state = SearchState::ResetRequested;
        return;
    }

    clearData();
    m_search_state = SearchState::None;

    m_next_search_offset = 0;
    search();
}

void ResourceModel::clearData()
{
    beginResetModel();
    m_packs.clear();
    endResetModel();
}

void ResourceModel::runSearchJob(Task::Ptr ptr)
{
    m_current_search_job = ptr;
    m_current_search_job->start();
}
void ResourceModel::runInfoJob(Task::Ptr ptr)
{
    if (!m_current_info_job.isRunning())
        m_current_info_job.clear();

    m_current_info_job.addTask(ptr);

    if (!m_current_info_job.isRunning())
        m_current_info_job.run();
}

std::optional<ResourceAPI::SortingMethod> ResourceModel::getCurrentSortingMethodByIndex() const
{
    std::optional<ResourceAPI::SortingMethod> sort{};

    {  // Find sorting method by ID
        auto sorting_methods = getSortingMethods();
        auto method = std::find_if(sorting_methods.constBegin(), sorting_methods.constEnd(),
                                   [this](auto const& e) { return m_current_sort_index == e.index; });
        if (method != sorting_methods.constEnd())
            sort = *method;
    }

    return sort;
}

std::optional<QIcon> ResourceModel::getIcon(QModelIndex& index, const QUrl& url)
{
    QPixmap pixmap;
    if (QPixmapCache::find(url.toString(), &pixmap))
        return { pixmap };

    if (!m_current_icon_job)
        m_current_icon_job.reset(new NetJob("IconJob", APPLICATION->network()));

    if (m_currently_running_icon_actions.contains(url))
        return {};
    if (m_failed_icon_actions.contains(url))
        return {};

    auto cache_entry = APPLICATION->metacache()->resolveEntry(
        metaEntryBase(),
        QString("logos/%1").arg(QString(QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Algorithm::Sha1).toHex())));
    auto icon_fetch_action = Net::Download::makeCached(url, cache_entry);

    auto full_file_path = cache_entry->getFullPath();
    connect(icon_fetch_action.get(), &NetAction::succeeded, this, [=] {
        auto icon = QIcon(full_file_path);
        QPixmapCache::insert(url.toString(), icon.pixmap(icon.actualSize({ 64, 64 })));

        m_currently_running_icon_actions.remove(url);

        emit dataChanged(index, index, { Qt::DecorationRole });
    });
    connect(icon_fetch_action.get(), &NetAction::failed, this, [=] {
        m_currently_running_icon_actions.remove(url);
        m_failed_icon_actions.insert(url);
    });

    m_currently_running_icon_actions.insert(url);

    m_current_icon_job->addNetAction(icon_fetch_action);
    if (!m_current_icon_job->isRunning())
        QMetaObject::invokeMethod(m_current_icon_job.get(), &NetJob::start);

    return {};
}

// No 'forgor to implement' shall pass here :blobfox_knife:
#define NEED_FOR_CALLBACK_ASSERT(name) \
    Q_ASSERT_X(0 != 0, #name, "You NEED to re-implement this if you intend on using the default callbacks.")

QJsonArray ResourceModel::documentToArray(QJsonDocument& doc) const
{
    NEED_FOR_CALLBACK_ASSERT("documentToArray");
    return {};
}
void ResourceModel::loadIndexedPack(ModPlatform::IndexedPack&, QJsonObject&)
{
    NEED_FOR_CALLBACK_ASSERT("loadIndexedPack");
}
void ResourceModel::loadExtraPackInfo(ModPlatform::IndexedPack&, QJsonObject&)
{
    NEED_FOR_CALLBACK_ASSERT("loadExtraPackInfo");
}
void ResourceModel::loadIndexedPackVersions(ModPlatform::IndexedPack&, QJsonArray&)
{
    NEED_FOR_CALLBACK_ASSERT("loadIndexedPackVersions");
}

/* Default callbacks */

void ResourceModel::searchRequestSucceeded(QJsonDocument& doc)
{
    QList<ModPlatform::IndexedPack> newList;
    auto packs = documentToArray(doc);

    for (auto packRaw : packs) {
        auto packObj = packRaw.toObject();

        ModPlatform::IndexedPack pack;
        try {
            loadIndexedPack(pack, packObj);
            newList.append(pack);
        } catch (const JSONValidationError& e) {
            qWarning() << "Error while loading resource from " << debugName() << ": " << e.cause();
            continue;
        }
    }

    if (packs.size() < 25) {
        m_search_state = SearchState::Finished;
    } else {
        m_next_search_offset += 25;
        m_search_state = SearchState::CanFetchMore;
    }

    // When you have a Qt build with assertions turned on, proceeding here will abort the application
    if (newList.size() == 0)
        return;

    beginInsertRows(QModelIndex(), m_packs.size(), m_packs.size() + newList.size() - 1);
    m_packs.append(newList);
    endInsertRows();
}

void ResourceModel::searchRequestFailed(QString reason, int network_error_code)
{
    switch (network_error_code) {
        default:
            // Network error
            QMessageBox::critical(nullptr, tr("Error"), tr("A network error occurred. Could not load mods."));
            break;
        case 409:
            // 409 Gone, notify user to update
            QMessageBox::critical(nullptr, tr("Error"),
                                  QString("%1").arg(tr("API version too old!\nPlease update %1!").arg(BuildConfig.LAUNCHER_DISPLAYNAME)));
            break;
    }

    m_search_state = SearchState::Finished;
}

void ResourceModel::searchRequestAborted()
{
    if (m_search_state != SearchState::ResetRequested)
        qCritical() << "Search task in" << debugName() << "aborted by an unknown reason!";

    // Retry fetching
    clearData();

    m_next_search_offset = 0;
    search();
}

void ResourceModel::versionRequestSucceeded(QJsonDocument& doc, ModPlatform::IndexedPack& pack, const QModelIndex& index)
{
    auto current_pack = data(index, Qt::UserRole).value<ModPlatform::IndexedPack>();

    // Check if the index is still valid for this resource or not
    if (pack.addonId != current_pack.addonId)
        return;

    try {
        auto arr = doc.isObject() ? Json::ensureArray(doc.object(), "data") : doc.array();
        loadIndexedPackVersions(current_pack, arr);
    } catch (const JSONValidationError& e) {
        qDebug() << doc;
        qWarning() << "Error while reading " << debugName() << " resource version: " << e.cause();
    }

    // Cache info :^)
    QVariant new_pack;
    new_pack.setValue(current_pack);
    if (!setData(index, new_pack, Qt::UserRole)) {
        qWarning() << "Failed to cache resource versions!";
        return;
    }

    emit versionListUpdated();
}

void ResourceModel::infoRequestSucceeded(QJsonDocument& doc, ModPlatform::IndexedPack& pack, const QModelIndex& index)
{
    auto current_pack = data(index, Qt::UserRole).value<ModPlatform::IndexedPack>();

    // Check if the index is still valid for this resource or not
    if (pack.addonId != current_pack.addonId)
        return;

    try {
        auto obj = Json::requireObject(doc);
        loadExtraPackInfo(current_pack, obj);
    } catch (const JSONValidationError& e) {
        qDebug() << doc;
        qWarning() << "Error while reading " << debugName() << " resource info: " << e.cause();
    }

    // Cache info :^)
    QVariant new_pack;
    new_pack.setValue(current_pack);
    if (!setData(index, new_pack, Qt::UserRole)) {
        qWarning() << "Failed to cache resource info!";
        return;
    }

    emit projectInfoUpdated();
}

}  // namespace ResourceDownload
