/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/namespace_string.h"

#include <ostream>

#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

constexpr auto listCollectionsCursorCol = "$cmd.listCollections"_sd;
constexpr auto bulkWriteCursorCol = "$cmd.bulkWrite"_sd;
constexpr auto collectionlessAggregateCursorCol = "$cmd.aggregate"_sd;
constexpr auto dropPendingNSPrefix = "system.drop."_sd;

constexpr auto fle2Prefix = "enxcol_."_sd;
constexpr auto fle2EscSuffix = ".esc"_sd;
constexpr auto fle2EccSuffix = ".ecc"_sd;
constexpr auto fle2EcocSuffix = ".ecoc"_sd;

}  // namespace


NamespaceString NamespaceString::parseFromStringExpectTenantIdInMultitenancyMode(StringData ns) {
    if (!gMultitenancySupport) {
        return NamespaceString(boost::none, ns);
    }

    auto tenantDelim = ns.find('_');
    auto collDelim = ns.find('.');
    // If the first '_' is after the '.' that separates the db and coll names, the '_' is part
    // of the coll name and is not a db prefix.
    if (tenantDelim == std::string::npos || collDelim < tenantDelim) {
        return NamespaceString(boost::none, ns);
    }

    auto swOID = OID::parse(ns.substr(0, tenantDelim));
    if (swOID.getStatus() == ErrorCodes::BadValue) {
        // If we fail to parse an OID, either the size of the substring is incorrect, or there is an
        // invalid character. This indicates that the db has the "_" character, but it does not act
        // as a delimeter for a tenantId prefix.
        return NamespaceString(boost::none, ns);
    }

    const TenantId tenantId(swOID.getValue());
    return NamespaceString(tenantId, ns.substr(tenantDelim + 1, ns.size() - 1 - tenantDelim));
}

bool NamespaceString::isListCollectionsCursorNS() const {
    return coll() == listCollectionsCursorCol;
}

bool NamespaceString::isCollectionlessAggregateNS() const {
    return coll() == collectionlessAggregateCursorCol;
}

bool NamespaceString::isLegalClientSystemNS(
    const ServerGlobalParams::FeatureCompatibility& currentFCV) const {
    auto collectionName = coll();
    if (isAdminDB()) {
        if (collectionName == "system.roles")
            return true;
        if (collectionName == kServerConfigurationNamespace.coll())
            return true;
        if (collectionName == kKeysCollectionNamespace.coll())
            return true;
        if (collectionName == "system.backup_users")
            return true;
    } else if (isConfigDB()) {
        if (collectionName == "system.sessions")
            return true;
        if (collectionName == kIndexBuildEntryNamespace.coll())
            return true;
        if (collectionName.find(".system.resharding.") != std::string::npos)
            return true;
        if (collectionName == kShardingDDLCoordinatorsNamespace.coll())
            return true;
        if (collectionName == kConfigsvrCoordinatorsNamespace.coll())
            return true;
    } else if (isLocalDB()) {
        if (collectionName == kSystemReplSetNamespace.coll())
            return true;
        if (collectionName == kLocalHealthLogNamespace.coll())
            return true;
        if (collectionName == kConfigsvrRestoreNamespace.coll())
            return true;
    }

    if (collectionName == "system.users")
        return true;
    if (collectionName == "system.js")
        return true;
    if (collectionName == kSystemDotViewsCollectionName)
        return true;
    if (isTemporaryReshardingCollection()) {
        return true;
    }
    if (isTimeseriesBucketsCollection() &&
        validCollectionName(collectionName.substr(kTimeseriesBucketsCollectionPrefix.size()))) {
        return true;
    }
    if (isChangeStreamPreImagesCollection()) {
        return true;
    }

    if (isChangeCollection()) {
        return true;
    }

    if (isSystemStatsCollection()) {
        return true;
    }

    return false;
}

/**
 * Oplog entries on 'system.views' should also be processed one at a time. View catalog immediately
 * reflects changes for each oplog entry so we can see inconsistent view catalog if multiple oplog
 * entries on 'system.views' are being applied out of the original order.
 *
 * Process updates to 'admin.system.version' individually as well so the secondary's FCV when
 * processing each operation matches the primary's when committing that operation.
 *
 * Process updates to 'config.shardMergeRecipients' individually so they serialize after
 * inserts into 'config.donatedFiles.<migrationId>'.
 *
 * Oplog entries on 'config.shards' should be processed one at a time, otherwise the in-memory state
 * that its kept on the TopologyTimeTicker might be wrong.
 *
 * Serialize updates to 'config.tenantMigrationDonors' and 'config.shardSplitDonors' to avoid races
 * with creating tenant access blockers on secondaries.
 */
bool NamespaceString::mustBeAppliedInOwnOplogBatch() const {
    auto ns = this->ns();
    return isSystemDotViews() || isServerConfigurationCollection() || isPrivilegeCollection() ||
        ns == kDonorReshardingOperationsNamespace.ns() ||
        ns == kForceOplogBatchBoundaryNamespace.ns() ||
        ns == kTenantMigrationDonorsNamespace.ns() || ns == kShardMergeRecipientsNamespace.ns() ||
        ns == kTenantMigrationRecipientsNamespace.ns() || ns == kShardSplitDonorsNamespace.ns() ||
        ns == kConfigsvrShardsNamespace.ns();
}

NamespaceString NamespaceString::makeBulkWriteNSS(const boost::optional<TenantId>& tenantId) {
    return NamespaceString(DatabaseName::kAdmin.db(), bulkWriteCursorCol, tenantId);
}

NamespaceString NamespaceString::makeClusterParametersNSS(
    const boost::optional<TenantId>& tenantId) {
    return tenantId ? NamespaceString(tenantId, DatabaseName::kConfig.db(), "clusterParameters")
                    : kClusterParametersNamespace;
}

NamespaceString NamespaceString::makeSystemDotViewsNamespace(const DatabaseName& dbName) {
    return NamespaceString(dbName, kSystemDotViewsCollectionName);
}

NamespaceString NamespaceString::makeSystemDotProfileNamespace(const DatabaseName& dbName) {
    return NamespaceString(dbName, kSystemDotProfileCollectionName);
}

NamespaceString NamespaceString::makeListCollectionsNSS(const DatabaseName& dbName) {
    NamespaceString nss(dbName, listCollectionsCursorCol);
    dassert(nss.isValid());
    dassert(nss.isListCollectionsCursorNS());
    return nss;
}

NamespaceString NamespaceString::makeGlobalConfigCollection(StringData collName) {
    return NamespaceString(DatabaseName::kConfig, collName);
}

NamespaceString NamespaceString::makeLocalCollection(StringData collName) {
    return NamespaceString(DatabaseName::kLocal, collName);
}

NamespaceString NamespaceString::makeCollectionlessAggregateNSS(const DatabaseName& dbName) {
    NamespaceString nss(dbName, collectionlessAggregateCursorCol);
    dassert(nss.isValid());
    dassert(nss.isCollectionlessAggregateNS());
    return nss;
}

NamespaceString NamespaceString::makeChangeCollectionNSS(
    const boost::optional<TenantId>& tenantId) {
    return NamespaceString{tenantId, DatabaseName::kConfig.db(), kChangeCollectionName};
}

NamespaceString NamespaceString::makeGlobalIndexNSS(const UUID& id) {
    return NamespaceString(DatabaseName::kSystem,
                           NamespaceString::kGlobalIndexCollectionPrefix + id.toString());
}

NamespaceString NamespaceString::makeMovePrimaryOplogBufferNSS(const UUID& migrationId) {
    return NamespaceString(DatabaseName::kConfig,
                           "movePrimaryOplogBuffer." + migrationId.toString());
}

NamespaceString NamespaceString::makeMovePrimaryCollectionsToCloneNSS(const UUID& migrationId) {
    return NamespaceString(DatabaseName::kConfig,
                           "movePrimaryCollectionsToClone." + migrationId.toString());
}

NamespaceString NamespaceString::makeMovePrimaryTempCollectionsPrefix(const UUID& migrationId) {
    return NamespaceString(DatabaseName::kConfig,
                           "movePrimaryRecipient." + migrationId.toString() + ".willBeDeleted.");
}

NamespaceString NamespaceString::makePreImageCollectionNSS(
    const boost::optional<TenantId>& tenantId) {
    return NamespaceString{tenantId, DatabaseName::kConfig.db(), kPreImagesCollectionName};
}

NamespaceString NamespaceString::makeReshardingLocalOplogBufferNSS(
    const UUID& existingUUID, const std::string& donorShardId) {
    return NamespaceString(DatabaseName::kConfig,
                           "localReshardingOplogBuffer." + existingUUID.toString() + "." +
                               donorShardId);
}

NamespaceString NamespaceString::makeReshardingLocalConflictStashNSS(
    const UUID& existingUUID, const std::string& donorShardId) {
    return NamespaceString(DatabaseName::kConfig,
                           "localReshardingConflictStash." + existingUUID.toString() + "." +
                               donorShardId);
}

NamespaceString NamespaceString::makeTenantUsersCollection(
    const boost::optional<TenantId>& tenantId) {
    return NamespaceString(tenantId, DatabaseName::kAdmin.db(), NamespaceString::kSystemUsers);
}

NamespaceString NamespaceString::makeTenantRolesCollection(
    const boost::optional<TenantId>& tenantId) {
    return NamespaceString(tenantId, DatabaseName::kAdmin.db(), NamespaceString::kSystemRoles);
}

NamespaceString NamespaceString::makeCommandNamespace(const DatabaseName& dbName) {
    return NamespaceString(dbName, "$cmd");
}

NamespaceString NamespaceString::makeDummyNamespace(const boost::optional<TenantId>& tenantId) {
    return NamespaceString(tenantId, DatabaseName::kConfig.db(), "dummy.namespace");
}

std::string NamespaceString::getSisterNS(StringData local) const {
    MONGO_verify(local.size() && local[0] != '.');
    return db().toString() + "." + local.toString();
}

void NamespaceString::serializeCollectionName(BSONObjBuilder* builder, StringData fieldName) const {
    if (isCollectionlessAggregateNS()) {
        builder->append(fieldName, 1);
    } else {
        builder->append(fieldName, coll());
    }
}

bool NamespaceString::isDropPendingNamespace() const {
    return coll().startsWith(dropPendingNSPrefix);
}

NamespaceString NamespaceString::makeDropPendingNamespace(const repl::OpTime& opTime) const {
    StringBuilder ss;
    ss << db() << "." << dropPendingNSPrefix;
    ss << opTime.getSecs() << "i" << opTime.getTimestamp().getInc() << "t" << opTime.getTerm();
    ss << "." << coll();
    return NamespaceString(ss.stringData());
}

StatusWith<repl::OpTime> NamespaceString::getDropPendingNamespaceOpTime() const {
    if (!isDropPendingNamespace()) {
        return Status(ErrorCodes::BadValue, fmt::format("Not a drop-pending namespace: {}", ns()));
    }

    auto collectionName = coll();
    auto opTimeBeginIndex = dropPendingNSPrefix.size();
    auto opTimeEndIndex = collectionName.find('.', opTimeBeginIndex);
    auto opTimeStr = std::string::npos == opTimeEndIndex
        ? collectionName.substr(opTimeBeginIndex)
        : collectionName.substr(opTimeBeginIndex, opTimeEndIndex - opTimeBeginIndex);

    auto incrementSeparatorIndex = opTimeStr.find('i');
    if (std::string::npos == incrementSeparatorIndex) {
        return Status(ErrorCodes::FailedToParse,
                      fmt::format("Missing 'i' separator in drop-pending namespace: {}", ns()));
    }

    auto termSeparatorIndex = opTimeStr.find('t', incrementSeparatorIndex);
    if (std::string::npos == termSeparatorIndex) {
        return Status(ErrorCodes::FailedToParse,
                      fmt::format("Missing 't' separator in drop-pending namespace: {}", ns()));
    }

    long long seconds;
    auto status = NumberParser{}(opTimeStr.substr(0, incrementSeparatorIndex), &seconds);
    if (!status.isOK()) {
        return status.withContext(
            fmt::format("Invalid timestamp seconds in drop-pending namespace: {}", ns()));
    }

    unsigned int increment;
    status = NumberParser{}(opTimeStr.substr(incrementSeparatorIndex + 1,
                                             termSeparatorIndex - (incrementSeparatorIndex + 1)),
                            &increment);
    if (!status.isOK()) {
        return status.withContext(
            fmt::format("Invalid timestamp increment in drop-pending namespace: {}", ns()));
    }

    long long term;
    status = mongo::NumberParser{}(opTimeStr.substr(termSeparatorIndex + 1), &term);
    if (!status.isOK()) {
        return status.withContext(fmt::format("Invalid term in drop-pending namespace: {}", ns()));
    }

    return repl::OpTime(Timestamp(Seconds(seconds), increment), term);
}

bool NamespaceString::isNamespaceAlwaysUnsharded() const {
    // Local and admin never have sharded collections
    if (isLocalDB() || isAdminDB())
        return true;

    // Config can only have the system.sessions as sharded
    if (isConfigDB())
        return *this != NamespaceString::kLogicalSessionsNamespace;

    if (isSystem()) {
        // Only some system collections (<DB>.system.<COLL>) can be sharded,
        // all the others are always unsharded.
        // This list does not contain 'config.system.sessions' because we already check it above
        return !isTemporaryReshardingCollection() && !isTimeseriesBucketsCollection();
    }

    return false;
}

bool NamespaceString::isConfigDotCacheDotChunks() const {
    return db() == "config" && coll().startsWith("cache.chunks.");
}

bool NamespaceString::isReshardingLocalOplogBufferCollection() const {
    return db() == "config" && coll().startsWith(kReshardingLocalOplogBufferPrefix);
}

bool NamespaceString::isReshardingConflictStashCollection() const {
    return db() == "config" && coll().startsWith(kReshardingConflictStashPrefix);
}

bool NamespaceString::isTemporaryReshardingCollection() const {
    return coll().startsWith(kTemporaryReshardingCollectionPrefix);
}

bool NamespaceString::isTimeseriesBucketsCollection() const {
    return coll().startsWith(kTimeseriesBucketsCollectionPrefix);
}

bool NamespaceString::isChangeStreamPreImagesCollection() const {
    return isConfigDB() && coll() == kPreImagesCollectionName;
}

bool NamespaceString::isChangeCollection() const {
    return isConfigDB() && coll() == kChangeCollectionName;
}

bool NamespaceString::isConfigImagesCollection() const {
    return ns() == kConfigImagesNamespace.ns();
}

bool NamespaceString::isConfigTransactionsCollection() const {
    return ns() == kSessionTransactionsTableNamespace.ns();
}

bool NamespaceString::isFLE2StateCollection() const {
    return coll().startsWith(fle2Prefix) &&
        (coll().endsWith(fle2EscSuffix) || coll().endsWith(fle2EccSuffix) ||
         coll().endsWith(fle2EcocSuffix));
}

bool NamespaceString::isOplogOrChangeCollection() const {
    return isOplog() || isChangeCollection();
}

bool NamespaceString::isSystemStatsCollection() const {
    return coll().startsWith(kStatisticsCollectionPrefix);
}

bool NamespaceString::isOutTmpBucketsCollection() const {
    return isTimeseriesBucketsCollection() &&
        getTimeseriesViewNamespace().coll().startsWith(kOutTmpCollectionPrefix);
}

NamespaceString NamespaceString::makeTimeseriesBucketsNamespace() const {
    return {dbName(), kTimeseriesBucketsCollectionPrefix.toString() + coll()};
}

NamespaceString NamespaceString::getTimeseriesViewNamespace() const {
    invariant(isTimeseriesBucketsCollection(), ns());
    return {dbName(), coll().substr(kTimeseriesBucketsCollectionPrefix.size())};
}

bool NamespaceString::isImplicitlyReplicated() const {
    if (isChangeStreamPreImagesCollection() || isConfigImagesCollection() || isChangeCollection()) {
        // Implicitly replicated namespaces are replicated, although they only replicate a subset of
        // writes.
        invariant(isReplicated());
        return true;
    }

    return false;
}

bool NamespaceString::isReplicated() const {
    if (isLocalDB()) {
        return false;
    }

    // Of collections not in the `local` database, only `system` collections might not be
    // replicated.
    if (!isSystem()) {
        return true;
    }

    if (isSystemDotProfile()) {
        return false;
    }

    // E.g: `system.version` is replicated.
    return true;
}

Status NamespaceStringOrUUID::isNssValid() const {
    if (const NamespaceString* nss = get_if<NamespaceString>(&_nssOrUUID)) {
        // _nss is set and not valid.
        if (!nss->isValid()) {
            return {ErrorCodes::InvalidNamespace,
                    fmt::format("Namespace {} is not a valid collection name",
                                nss->toStringForErrorMsg())};
        }
    }

    return Status::OK();
}

std::string NamespaceStringOrUUID::toStringForErrorMsg() const {
    if (const NamespaceString* nss = get_if<NamespaceString>(&_nssOrUUID)) {
        return nss->toStringForErrorMsg();
    }

    return get<1>(get<UUIDWithDbName>(_nssOrUUID)).toString();
}

std::string toStringForLogging(const NamespaceStringOrUUID& nssOrUUID) {
    if (auto nss = nssOrUUID.nss()) {
        return toStringForLogging(nss.get());
    } else {
        return nssOrUUID.uuid()->toString();
    }
}

void NamespaceStringOrUUID::serialize(BSONObjBuilder* builder, StringData fieldName) const {
    if (const NamespaceString* nss = get_if<NamespaceString>(&_nssOrUUID)) {
        builder->append(fieldName, nss->coll());
    } else {
        get<1>(get<UUIDWithDbName>(_nssOrUUID)).appendToBuilder(builder, fieldName);
    }
}

}  // namespace mongo
