/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/timeseries/bucket_catalog/closed_bucket.h"
#include "mongo/db/storage/storage_parameters_gen.h"

namespace mongo::timeseries::bucket_catalog {

ClosedBucket::~ClosedBucket() {
    if (_bucketStateRegistry) {
        removeDirectWrite(*_bucketStateRegistry, bucketId);
    }
}

ClosedBucket::ClosedBucket(BucketStateRegistry* bsr,
                           const BucketId& bucketId,
                           const std::string& tf,
                           boost::optional<uint32_t> nm)
    : bucketId{bucketId}, timeField{tf}, numMeasurements{nm}, _bucketStateRegistry{bsr} {
    invariant(_bucketStateRegistry);

    // When enabled, we skip constructing ClosedBuckets as we don't need to compress the bucket.
    invariant(!feature_flags::gTimeseriesAlwaysUseCompressedBuckets.isEnabled(
        serverGlobalParams.featureCompatibility));

    addDirectWrite(*_bucketStateRegistry, bucketId, /*stopTracking*/ true);
}

ClosedBucket::ClosedBucket(ClosedBucket&& other)
    : bucketId{std::move(other.bucketId)},
      timeField{std::move(other.timeField)},
      numMeasurements{other.numMeasurements},
      _bucketStateRegistry{other._bucketStateRegistry} {
    other._bucketStateRegistry = nullptr;
}

ClosedBucket& ClosedBucket::operator=(ClosedBucket&& other) {
    if (this != &other) {
        bucketId = std::move(other.bucketId);
        timeField = std::move(other.timeField);
        numMeasurements = other.numMeasurements;
        _bucketStateRegistry = other._bucketStateRegistry;
        other._bucketStateRegistry = nullptr;
    }
    return *this;
}

}  // namespace mongo::timeseries::bucket_catalog
