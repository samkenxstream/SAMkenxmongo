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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"

namespace mongo {

/**
 * Service to manage DDL locks.
 */
class DDLLockManager {

    /**
     * ScopedBaseDDLLock will hold a DDL lock for the given resource without performing any check.
     */
    class ScopedBaseDDLLock {
        ScopedBaseDDLLock(const ScopedBaseDDLLock&) = delete;
        ScopedBaseDDLLock& operator=(const ScopedBaseDDLLock&) = delete;

        ScopedBaseDDLLock(OperationContext* opCtx,
                          StringData resName,
                          const ResourceId& resId,
                          StringData reason,
                          LockMode mode,
                          Milliseconds timeout,
                          bool waitForRecovery);

    public:
        ScopedBaseDDLLock(OperationContext* opCtx,
                          const NamespaceString& ns,
                          StringData reason,
                          LockMode mode,
                          Milliseconds timeout,
                          bool waitForRecovery);

        ScopedBaseDDLLock(OperationContext* opCtx,
                          const DatabaseName& db,
                          StringData reason,
                          LockMode mode,
                          Milliseconds timeout,
                          bool waitForRecovery);

        virtual ~ScopedBaseDDLLock();

        ScopedBaseDDLLock(ScopedBaseDDLLock&& other);

        StringData getResourceName() const {
            return _resourceName;
        }
        StringData getReason() const {
            return _reason;
        }

    protected:
        const std::string _resourceName;
        const ResourceId _resourceId;
        const std::string _reason;
        const LockMode _mode;
        DDLLockManager* _lockManager;
    };

public:
    // Default timeout which will be used if one is not passed to the lock method.
    static const Minutes kDefaultLockTimeout;

    // Timeout value, which specifies that if the lock is not available immediately, no attempt
    // should be made to wait for it to become free.
    static const Milliseconds kSingleLockAttemptTimeout;

    // RAII-style class to acquire a DDL lock on the given database
    class ScopedDatabaseDDLLock : public ScopedBaseDDLLock {
    public:
        /**
         * Constructs a ScopedDatabaseDDLLock object
         *
         * @db      Database to lock.
         * @reason 	Reason for which the lock is being acquired (e.g. 'createCollection').
         * @mode    Lock mode.
         * @timeout Time after which this acquisition attempt will give up in case of lock
         * contention. A timeout value of -1 means the acquisition will be retried forever.
         *
         * Throws:
         *     ErrorCodes::LockBusy in case the timeout is reached.
         *     ErrorCodes::LockTimeout when not being on kPrimaryAndRecovered state and timeout
         *         is reached
         *     ErrorCategory::Interruption in case the operation context is interrupted.
         *     ErrorCodes::IllegalOperation in case of not being on the db primary shard
         *
         * Note that object can only be instantiated from the replica set primary node of the
         * db primary shard. It's caller's responsability to release the acquired locks on
         * step-downs
         */
        ScopedDatabaseDDLLock(OperationContext* opCtx,
                              const DatabaseName& db,
                              StringData reason,
                              LockMode mode,
                              Milliseconds timeout = kDefaultLockTimeout);
    };

    // RAII-style class to acquire a DDL lock on the given collection
    class ScopedCollectionDDLLock : public ScopedBaseDDLLock {
    public:
        /**
         * Constructs a ScopedCollectionDDLLock object
         *
         * @ns      Collection to lock.
         * @reason 	Reason for which the lock is being acquired (e.g. 'createCollection').
         * @mode    Lock mode.
         * @timeout Time after which this acquisition attempt will give up in case of lock
         * contention. A timeout value of -1 means the acquisition will be retried forever.
         *
         * Throws:
         *     ErrorCodes::LockBusy in case the timeout is reached.
         *     ErrorCodes::LockTimeout when not being on kPrimaryAndRecovered state and timeout
         *         is reached
         *     ErrorCategory::Interruption in case the operation context is interrupted.
         *     ErrorCodes::IllegalOperation in case of not being on the db primary shard
         *
         * Note that object can only be instantiated from the replica set primary node of the
         * db primary shard. It's caller's responsability to release the acquired locks on
         * step-downs
         */
        ScopedCollectionDDLLock(OperationContext* opCtx,
                                const NamespaceString& ns,
                                StringData reason,
                                LockMode mode,
                                Milliseconds timeout = kDefaultLockTimeout);
    };

    DDLLockManager() = default;
    ~DDLLockManager() = default;

    /**
     * Retrieves the DDLLockManager singleton.
     */
    static DDLLockManager* get(ServiceContext* service);
    static DDLLockManager* get(OperationContext* opCtx);

protected:
    struct NSLock {
        NSLock(StringData reason) : reason(reason.toString()) {}

        stdx::condition_variable cvLocked;
        int numWaiting = 1;
        bool isInProgress = true;
        std::string reason;
    };

    Mutex _mutex = MONGO_MAKE_LATCH("DDLLockManager::_mutex");
    StringMap<std::shared_ptr<NSLock>> _inProgressMap;

    enum class State {
        /**
         * When the node become secondary the state is set to kPaused and all the lock acquisitions
         * will be blocked except if the request comes from a DDLCoordinator.
         */
        kPaused,

        /**
         * After the node became primary and the ShardingDDLCoordinatorService already re-acquired
         * all the previously acquired DDL locks for ongoing DDL coordinators the state transition
         * to kPrimaryAndRecovered and the lock acquisitions are unblocked.
         */
        kPrimaryAndRecovered,
    };

    State _state = State::kPaused;
    mutable stdx::condition_variable _stateCV;

    void setState(const State& state);

    void _lock(OperationContext* opCtx,
               StringData ns,
               const ResourceId& resId,
               StringData reason,
               LockMode mode,
               Milliseconds timeout,
               bool waitForRecovery);

    void _unlock(StringData ns, const ResourceId& resId, StringData reason);

    friend class ShardingDDLCoordinatorService;
    friend class ShardingDDLCoordinator;
    friend class ShardingDDLCoordinatorServiceTest;
    friend class ShardingCatalogManager;
};

}  // namespace mongo
