// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cache/object_cache/starcache_module.h"

#include <gtest/gtest.h>

#include "cache/block_cache/test_cache_utils.h"
#include "cache/starcache_engine.h"
#include "fs/fs_util.h"
#include "testutil/assert.h"

namespace starrocks {
class StarCacheModuleTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_OK(fs::create_directories(cache_dir));

        _init_local_cache();
        _cache = std::make_shared<StarCacheModule>(_local_cache->starcache_instance());
    }
    void TearDown() override {
        ASSERT_OK(_local_cache->shutdown());
        ASSERT_OK(fs::remove_all(cache_dir));
    }

    static void Deleter(const CacheKey& k, void* v) { free(v); }
    void _init_local_cache();

    static std::string int_to_string(size_t length, int num) {
        std::ostringstream oss;
        oss << std::setw(length) << std::setfill('0') << num;
        return oss.str();
    }

    void _check_not_found(int value) {
        std::string key = int_to_string(6, value);
        ObjectCacheHandlePtr handle = nullptr;
        Status st = _cache->lookup(key, &handle, nullptr);
        ASSERT_TRUE(st.is_not_found());
    }

    void _check_found(int value) {
        std::string key = int_to_string(6, value);
        ObjectCacheHandlePtr handle = nullptr;
        ASSERT_OK(_cache->lookup(key, &handle, nullptr));
        ASSERT_EQ(*(int*)(_cache->value(handle)), value);
        _cache->release(handle);
    }

    void insert_value(int i);

    std::string cache_dir = "./starcache_module_test";
    std::shared_ptr<StarCacheEngine> _local_cache;
    std::shared_ptr<ObjectCache> _cache;
    ObjectCacheWriteOptions _write_opt;
    size_t _value_size = 256 * 1024;
    int64_t _mem_quota = 64 * 1024 * 1024;
};

void StarCacheModuleTest::_init_local_cache() {
    CacheOptions options = TestCacheUtils::create_simple_options(256 * KB, _mem_quota);
    options.dir_spaces.push_back({.path = cache_dir, .size = 50 * MB});

    _local_cache = std::make_shared<StarCacheEngine>();
    ASSERT_OK(_local_cache->init(options));
}

void StarCacheModuleTest::insert_value(int i) {
    std::string key = int_to_string(6, i);
    int* ptr = (int*)malloc(_value_size);
    *ptr = i;
    ObjectCacheHandlePtr handle = nullptr;
    ASSERT_OK(_cache->insert(key, (void*)ptr, _value_size, &Deleter, &handle, _write_opt));
    _cache->release(handle);
}

TEST_F(StarCacheModuleTest, insert_success) {
    insert_value(0);
    size_t kv_size = _cache->usage();

    for (int i = 1; i < 20; i++) {
        insert_value(i);
    }
    ASSERT_EQ(_cache->usage(), kv_size * 20);
}

TEST_F(StarCacheModuleTest, test_charge_size) {
    size_t mem_size = 4096;
    void* ptr = malloc(mem_size);
    int* value = new (ptr) int;
    *value = 10;
    ObjectCacheHandlePtr handle = nullptr;
    ObjectCacheWriteOptions opts;
    ASSERT_OK(_cache->insert("1", (void*)ptr, mem_size, &Deleter, &handle, opts));
    _cache->release(handle);

    ObjectCacheHandlePtr lookup_handle = nullptr;
    ASSERT_OK(_cache->lookup("1", &lookup_handle, nullptr));
    auto value_slice = _cache->value_slice(lookup_handle);
    ASSERT_EQ(value_slice.size, mem_size);
    ASSERT_EQ(*(int*)(value_slice.data), 10);
    _cache->release(lookup_handle);
}

TEST_F(StarCacheModuleTest, test_charge_size_with_write_option) {
    size_t mem_size = 4096;
    void* ptr = malloc(mem_size);
    int* value = new (ptr) int;
    *value = 10;
    ObjectCacheHandlePtr handle = nullptr;
    ASSERT_OK(_cache->insert("1", (void*)ptr, mem_size, &Deleter, &handle, _write_opt));
    _cache->release(handle);

    ObjectCacheHandlePtr lookup_handle = nullptr;
    ASSERT_OK(_cache->lookup("1", &lookup_handle, nullptr));
    auto value_slice = _cache->value_slice(lookup_handle);
    ASSERT_EQ(value_slice.size, mem_size);
    ASSERT_EQ(*(int*)(value_slice.data), 10);
    _cache->release(lookup_handle);
}

TEST_F(StarCacheModuleTest, insert_with_null_options) {
    std::string key = int_to_string(6, 0);
    int* ptr = (int*)malloc(_value_size);
    *ptr = 0;
    ObjectCacheHandlePtr handle = nullptr;
    ObjectCacheWriteOptions opts;
    ASSERT_OK(_cache->insert(key, (void*)ptr, _value_size, &Deleter, &handle, opts));
    _cache->release(handle);
}

TEST_F(StarCacheModuleTest, insert_and_release_old_handle) {
    std::string key = int_to_string(6, 0);
    int* ptr = (int*)malloc(_value_size);
    *ptr = 0;
    ObjectCacheHandlePtr handle = nullptr;
    ASSERT_OK(_cache->insert(key, (void*)ptr, _value_size, &Deleter, &handle, _write_opt));

    key = int_to_string(6, 1);
    ptr = (int*)malloc(_value_size);
    *ptr = 1;
    ASSERT_OK(_cache->insert(key, (void*)ptr, _value_size, &Deleter, &handle, _write_opt));
    _cache->release(handle);
}

TEST_F(StarCacheModuleTest, lookup) {
    insert_value(0);
    insert_value(1);

    ObjectCacheHandlePtr handle = nullptr;
    std::string key = int_to_string(6, 1);
    ASSERT_OK(_cache->lookup(key, &handle, nullptr));
    ASSERT_EQ(*(int*)_cache->value(handle), 1);

    ASSERT_OK(_cache->lookup(key, &handle, nullptr));
    ASSERT_EQ(*(int*)_cache->value(handle), 1);
    _cache->release(handle);

    _check_not_found(2);
}

TEST_F(StarCacheModuleTest, remove) {
    insert_value(0);
    insert_value(1);

    ASSERT_OK(_cache->remove(int_to_string(6, 1)));
    _check_not_found(1);
}

TEST_F(StarCacheModuleTest, value_slice) {
    insert_value(1);

    ObjectCacheHandlePtr handle = nullptr;
    std::string key = int_to_string(6, 1);
    ASSERT_OK(_cache->lookup(key, &handle, nullptr));
    Slice slice = _cache->value_slice(handle);
    ASSERT_EQ(*(int*)slice.data, 1);
    _cache->release(handle);
}

TEST_F(StarCacheModuleTest, set_capacity) {
    insert_value(0);
    size_t kv_size = _cache->usage();

    size_t num = _mem_quota / kv_size;

    for (size_t i = 1; i < num; i++) {
        insert_value(i);
    }
    _check_found(0);
    ASSERT_LE(_cache->usage(), num * kv_size);
    ASSERT_EQ(_cache->capacity(), _mem_quota);

    ASSERT_OK(_cache->set_capacity(_mem_quota / 2));
    _check_not_found(1);
    ASSERT_EQ(_cache->capacity(), _mem_quota / 2);
    ASSERT_LE(_cache->usage(), _mem_quota / 2);
}

TEST_F(StarCacheModuleTest, adjust_capacity) {
    insert_value(0);
    size_t kv_size = _cache->usage();

    size_t num = _mem_quota / kv_size;

    for (size_t i = 1; i < num; i++) {
        insert_value(i);
    }
    _check_found(0);
    ASSERT_LE(_cache->usage(), _mem_quota);
    ASSERT_EQ(_cache->capacity(), _mem_quota);

    ASSERT_OK(_cache->adjust_capacity(-1 * _mem_quota / 2, 0));
    _check_not_found(1);
    ASSERT_LE(_cache->usage(), _mem_quota / 2);
    ASSERT_EQ(_cache->capacity(), _mem_quota / 2);

    ASSERT_TRUE(_cache->adjust_capacity(-1 * _mem_quota / 3, _mem_quota / 2).is_invalid_argument());
}

TEST_F(StarCacheModuleTest, metrics) {
    insert_value(0);
    size_t kv_size = _cache->usage();

    for (size_t i = 1; i < 128; i++) {
        insert_value(i);
    }
    for (size_t i = 0; i < 10; i++) {
        _check_found(i);
    }
    for (size_t i = 200; i < 210; i++) {
        _check_not_found(i);
    }

    ASSERT_EQ(_cache->capacity(), _mem_quota);
    ASSERT_EQ(_cache->usage(), kv_size * 128);
    ASSERT_EQ(_cache->lookup_count(), 20);
    ASSERT_EQ(_cache->hit_count(), 10);

    auto metrics = _cache->metrics();
    ASSERT_EQ(metrics.capacity, _mem_quota);
    ASSERT_EQ(metrics.usage, kv_size * 128);
    ASSERT_EQ(metrics.lookup_count, 20);
    ASSERT_EQ(metrics.hit_count, 10);
    ASSERT_EQ(metrics.object_item_count, 128);
}

TEST_F(StarCacheModuleTest, prune) {
    for (size_t i = 0; i < 128; i++) {
        insert_value(i);
    }
    ASSERT_OK(_cache->prune());
    ASSERT_EQ(_cache->usage(), 0);
}
} // namespace starrocks