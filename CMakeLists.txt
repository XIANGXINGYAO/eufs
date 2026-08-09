cmake_minimum_required(VERSION 3.16)
project(eufs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

option(EUFS_BUILD_BRPC_SERVICE "Build the Apache brpc object service" OFF)
set(BRPC_ROOT "" CACHE PATH "Apache brpc output root containing include/ and lib/")

add_library(eufs_mounted_image_session
  src/storage/mounted_image_session.cpp
)
target_include_directories(eufs_mounted_image_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(eufs_mounted_image_session PRIVATE -Wall -Wextra -Wpedantic)

add_library(eufs_ondisk
  src/metadata/ondisk_format.cpp
)
target_include_directories(eufs_ondisk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(eufs_ondisk PRIVATE -Wall -Wextra -Wpedantic)

add_library(eufs_mkfs_core
  src/storage/mkfs.cpp
)
target_include_directories(eufs_mkfs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_mkfs_core PUBLIC eufs_ondisk)
target_compile_options(eufs_mkfs_core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(eufs_mkfs_core PRIVATE _FILE_OFFSET_BITS=64)

add_library(eufs_image_reader
  src/storage/image_reader.cpp
)
target_include_directories(eufs_image_reader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_image_reader PUBLIC eufs_ondisk)
target_compile_options(eufs_image_reader PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(eufs_image_reader PRIVATE _FILE_OFFSET_BITS=64)

add_library(eufs_consistency_checker
  src/checker/consistency_checker.cpp
)
target_include_directories(eufs_consistency_checker PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_consistency_checker PUBLIC
  eufs_ondisk eufs_request_ledger_format
)
target_compile_options(eufs_consistency_checker PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(eufs_consistency_checker PRIVATE _FILE_OFFSET_BITS=64)

add_library(eufs_bitmap_allocator
  src/storage/bitmap_allocator.cpp
)
target_include_directories(eufs_bitmap_allocator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(eufs_bitmap_allocator PRIVATE -Wall -Wextra -Wpedantic)

add_library(eufs_create_plan
  src/metadata/empty_file_create_plan.cpp
)
target_include_directories(eufs_create_plan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_create_plan PUBLIC eufs_image_reader eufs_bitmap_allocator eufs_ondisk)
target_compile_options(eufs_create_plan PRIVATE -Wall -Wextra -Wpedantic)

add_library(eufs_file_mutation_plan
  src/metadata/file_mutation_plan.cpp
)
target_include_directories(eufs_file_mutation_plan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_file_mutation_plan PUBLIC eufs_image_reader eufs_bitmap_allocator eufs_ondisk)
target_compile_options(eufs_file_mutation_plan PRIVATE -Wall -Wextra -Wpedantic)

add_library(eufs_file_write_plan
  src/metadata/file_write_plan.cpp
)
target_include_directories(eufs_file_write_plan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_file_write_plan PUBLIC eufs_file_mutation_plan)
target_compile_options(eufs_file_write_plan PRIVATE -Wall -Wextra -Wpedantic)

add_library(eufs_new_object_plan
  src/metadata/new_object_plan.cpp
)
target_include_directories(eufs_new_object_plan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_new_object_plan PUBLIC eufs_image_reader eufs_bitmap_allocator eufs_ondisk)
target_compile_options(eufs_new_object_plan PRIVATE -Wall -Wextra -Wpedantic)

add_library(eufs_object_replace_plan
  src/metadata/object_replace_plan.cpp
)
target_include_directories(eufs_object_replace_plan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_object_replace_plan PUBLIC eufs_file_mutation_plan)
target_compile_options(eufs_object_replace_plan PRIVATE -Wall -Wextra -Wpedantic)

add_library(eufs_request_fingerprint
  src/object/request_fingerprint.cpp
)
target_include_directories(eufs_request_fingerprint PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(eufs_request_fingerprint PUBLIC
  eufs_ondisk OpenSSL::Crypto
)
target_compile_options(eufs_request_fingerprint PRIVATE
  -Wall -Wextra -Wpedantic
)

add_library(eufs_request_ledger_format
  src/object/request_ledger_format.cpp
)
target_include_directories(eufs_request_ledger_format PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(eufs_request_ledger_format PUBLIC
  eufs_ondisk
)
target_compile_options(eufs_request_ledger_format PRIVATE
  -Wall -Wextra -Wpedantic
)

add_library(eufs_request_ledger_index
  src/object/request_ledger_index.cpp
)
target_include_directories(eufs_request_ledger_index PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(eufs_request_ledger_index PUBLIC
  eufs_request_ledger_format eufs_image_reader
)
target_compile_options(eufs_request_ledger_index PRIVATE
  -Wall -Wextra -Wpedantic
)

add_library(eufs_request_ledger_plan
  src/object/request_ledger_plan.cpp
)
target_include_directories(eufs_request_ledger_plan PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(eufs_request_ledger_plan PUBLIC
  eufs_request_ledger_index eufs_image_reader
)
target_compile_options(eufs_request_ledger_plan PRIVATE
  -Wall -Wextra -Wpedantic
)

add_library(eufs_journal_format
  src/journal/ondisk_journal.cpp
)
target_include_directories(eufs_journal_format PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_journal_format PUBLIC eufs_ondisk)
target_compile_options(eufs_journal_format PRIVATE -Wall -Wextra -Wpedantic)

add_library(eufs_ring_reservation
  src/journal/ring_reservation.cpp
)
target_include_directories(eufs_ring_reservation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_ring_reservation PUBLIC eufs_journal_format)
target_compile_options(eufs_ring_reservation PRIVATE -Wall -Wextra -Wpedantic)

add_library(eufs_journal_control_store
  src/journal/durable_stage_failpoint.cpp
  src/journal/journal_control_store.cpp
  src/journal/journal_recovery.cpp
)
target_include_directories(eufs_journal_control_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_journal_control_store PUBLIC eufs_ring_reservation)
target_compile_options(eufs_journal_control_store PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(eufs_journal_control_store PRIVATE _FILE_OFFSET_BITS=64)

add_library(eufs_journal_transaction_executor
  src/journal/journal_transaction_executor.cpp
)
target_include_directories(eufs_journal_transaction_executor PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(eufs_journal_transaction_executor PUBLIC
  eufs_journal_control_store eufs_mounted_image_session
)
target_compile_options(eufs_journal_transaction_executor PRIVATE
  -Wall -Wextra -Wpedantic
)
target_compile_definitions(eufs_journal_transaction_executor PRIVATE
  _FILE_OFFSET_BITS=64
)

add_library(eufs_object_backend
  src/object/object_backend.cpp
)
target_include_directories(eufs_object_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_object_backend PUBLIC
  eufs_new_object_plan eufs_object_replace_plan eufs_journal_transaction_executor
  eufs_mounted_image_session eufs_image_reader eufs_request_ledger_index
  eufs_request_ledger_plan
  Threads::Threads
)
target_compile_options(eufs_object_backend PRIVATE -Wall -Wextra -Wpedantic)

add_library(eufs_rpc_admission
  src/rpc/inflight_byte_limiter.cpp
  src/rpc/bounded_task_queue.cpp
)
target_include_directories(eufs_rpc_admission PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(eufs_rpc_admission PUBLIC Threads::Threads)
target_compile_options(eufs_rpc_admission PRIVATE
  -Wall -Wextra -Wpedantic
)

if(EUFS_BUILD_BRPC_SERVICE)
  if(NOT BRPC_ROOT)
    message(FATAL_ERROR "BRPC_ROOT is required when EUFS_BUILD_BRPC_SERVICE=ON")
  endif()
  find_package(Protobuf REQUIRED)
  find_package(OpenSSL REQUIRED)
  find_path(BRPC_INCLUDE_DIR brpc/server.h
    PATHS ${BRPC_ROOT}/include NO_DEFAULT_PATH REQUIRED
  )
  find_library(BRPC_LIBRARY brpc
    PATHS ${BRPC_ROOT}/lib NO_DEFAULT_PATH REQUIRED
  )
  pkg_check_modules(BRPC_PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
  pkg_check_modules(BRPC_GFLAGS REQUIRED IMPORTED_TARGET gflags)
  pkg_check_modules(BRPC_LEVELDB REQUIRED IMPORTED_TARGET leveldb)

  protobuf_generate_cpp(EUFS_RPC_PROTO_SRCS EUFS_RPC_PROTO_HDRS
    src/rpc/object_service.proto
  )
  add_library(eufs_rpc_protocol
    ${EUFS_RPC_PROTO_SRCS}
    ${EUFS_RPC_PROTO_HDRS}
  )
  target_include_directories(eufs_rpc_protocol PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
  )
  target_link_libraries(eufs_rpc_protocol PUBLIC PkgConfig::BRPC_PROTOBUF)

  add_library(eufs_object_service
    src/rpc/object_service_metrics.cpp
    src/rpc/object_service_impl.cpp
  )
  target_include_directories(eufs_object_service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
  )
  target_include_directories(eufs_object_service SYSTEM PUBLIC
    ${BRPC_INCLUDE_DIR}
  )
  target_compile_definitions(eufs_object_service PRIVATE
    __const__=__unused__
  )
  target_compile_options(eufs_object_service PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_link_libraries(eufs_object_service PUBLIC
    eufs_rpc_protocol eufs_rpc_admission eufs_object_backend
    eufs_request_fingerprint
    ${BRPC_LIBRARY}
    PkgConfig::BRPC_GFLAGS PkgConfig::BRPC_LEVELDB
    PkgConfig::BRPC_PROTOBUF OpenSSL::SSL OpenSSL::Crypto
    ${CMAKE_DL_LIBS} rt
  )

  add_executable(eufs_object_server
    src/app/eufs_object_server_main.cpp
  )
  target_include_directories(eufs_object_server SYSTEM PRIVATE
    ${BRPC_INCLUDE_DIR}
  )
  target_compile_definitions(eufs_object_server PRIVATE
    __const__=__unused__
  )
  target_compile_options(eufs_object_server PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_link_libraries(eufs_object_server PRIVATE eufs_object_service)

  add_executable(eufs_object_client
    src/app/eufs_object_client_main.cpp
  )
  target_include_directories(eufs_object_client SYSTEM PRIVATE
    ${BRPC_INCLUDE_DIR}
  )
  target_compile_definitions(eufs_object_client PRIVATE
    __const__=__unused__
  )
  target_compile_options(eufs_object_client PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_link_libraries(eufs_object_client PRIVATE
    eufs_rpc_protocol ${BRPC_LIBRARY}
    PkgConfig::BRPC_GFLAGS PkgConfig::BRPC_LEVELDB
    PkgConfig::BRPC_PROTOBUF OpenSSL::SSL OpenSSL::Crypto
    ${CMAKE_DL_LIBS} rt Threads::Threads
  )
endif()

target_link_libraries(eufs_mkfs_core PUBLIC eufs_journal_format)
target_link_libraries(eufs_image_reader PUBLIC eufs_journal_format)
target_link_libraries(eufs_consistency_checker PUBLIC eufs_journal_format)

add_library(eufsck_core
  src/checker/consistency_report.cpp
  src/checker/eufsck_command.cpp
)
target_include_directories(eufsck_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufsck_core PUBLIC eufs_consistency_checker)
target_compile_options(eufsck_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(eufsck src/app/eufsck_main.cpp)
target_link_libraries(eufsck PRIVATE eufsck_core)
target_compile_options(eufsck PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(eufsck PRIVATE _FILE_OFFSET_BITS=64)

add_library(eufs_mount_state
  src/fuse/mount_state.cpp
)
target_include_directories(eufs_mount_state PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_mount_state PUBLIC eufs_mounted_image_session eufs_journal_control_store eufs_image_reader)
target_compile_options(eufs_mount_state PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(eufs_mount_state PRIVATE _FILE_OFFSET_BITS=64)

add_executable(eufs_mkfs src/app/mkfs_main.cpp)
set_target_properties(eufs_mkfs PROPERTIES OUTPUT_NAME "eufs-mkfs")
target_link_libraries(eufs_mkfs PRIVATE eufs_mkfs_core)
target_compile_options(eufs_mkfs PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(eufs_mkfs PRIVATE _FILE_OFFSET_BITS=64)

add_executable(eufsd
  src/app/eufsd_main.cpp
  src/fuse/operations.cpp
)
target_include_directories(eufsd PRIVATE ${FUSE3_INCLUDE_DIRS})
target_link_directories(eufsd PRIVATE ${FUSE3_LIBRARY_DIRS})
target_link_libraries(eufsd PRIVATE
  eufs_mount_state eufs_create_plan eufs_file_write_plan
  eufs_journal_transaction_executor
  ${FUSE3_LIBRARIES}
)
target_compile_options(eufsd PRIVATE ${FUSE3_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)
target_compile_definitions(eufsd PRIVATE _FILE_OFFSET_BITS=64)

include(CTest)
if(BUILD_TESTING)
  enable_testing()

  add_executable(durable_stage_failpoint_test
    tests/durable_stage_failpoint_test.cpp
  )
  target_link_libraries(durable_stage_failpoint_test PRIVATE
    eufs_journal_control_store
  )
  target_compile_options(durable_stage_failpoint_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  add_test(NAME durable_stage_failpoint_test
    COMMAND durable_stage_failpoint_test
  )

  # 下面两个库只用于构造测试镜像和早期恢复场景，不进入 eufsd 产品链路。
  add_library(eufs_first_block_write_plan
    tests/support/first_block_write_plan.cpp
  )
  target_include_directories(eufs_first_block_write_plan PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(eufs_first_block_write_plan PUBLIC
    eufs_image_reader eufs_bitmap_allocator eufs_ondisk
  )
  target_compile_options(eufs_first_block_write_plan PRIVATE
    -Wall -Wextra -Wpedantic
  )

  add_library(eufs_writable_image
    tests/support/writable_image.cpp
  )
  target_include_directories(eufs_writable_image PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(eufs_writable_image PUBLIC
    eufs_create_plan eufs_first_block_write_plan eufs_file_write_plan
    eufs_ondisk
  )
  target_compile_options(eufs_writable_image PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(eufs_writable_image PRIVATE
    _FILE_OFFSET_BITS=64
  )

  add_executable(ondisk_format_test tests/ondisk_format_test.cpp)
  target_link_libraries(ondisk_format_test PRIVATE eufs_ondisk)
  target_compile_options(ondisk_format_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME ondisk_format_test COMMAND ondisk_format_test)

  add_executable(ondisk_journal_test tests/ondisk_journal_test.cpp)
  target_link_libraries(ondisk_journal_test PRIVATE eufs_journal_format)
  target_compile_options(ondisk_journal_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME ondisk_journal_test COMMAND ondisk_journal_test)

  add_executable(mkfs_image_test tests/mkfs_image_test.cpp)
  target_link_libraries(mkfs_image_test PRIVATE
    eufs_mkfs_core eufs_image_reader eufs_consistency_checker
  )
  target_compile_options(mkfs_image_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(mkfs_image_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME mkfs_image_test COMMAND mkfs_image_test)

  add_executable(image_reader_test tests/image_reader_test.cpp)
  target_link_libraries(image_reader_test PRIVATE eufs_image_reader eufs_mkfs_core)
  target_compile_options(image_reader_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(image_reader_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME image_reader_test COMMAND image_reader_test)

  add_executable(consistency_checker_root_seed_test
    tests/consistency_checker_root_seed_test.cpp
  )
  target_link_libraries(consistency_checker_root_seed_test PRIVATE
    eufs_consistency_checker eufs_mkfs_core
  )
  target_compile_options(consistency_checker_root_seed_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(consistency_checker_root_seed_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME consistency_checker_root_seed_test
    COMMAND consistency_checker_root_seed_test
  )

  add_executable(consistency_checker_root_directory_test
    tests/consistency_checker_root_directory_test.cpp
  )
  target_link_libraries(consistency_checker_root_directory_test PRIVATE
    eufs_consistency_checker eufs_writable_image eufs_image_reader
    eufs_mkfs_core
  )
  target_compile_options(consistency_checker_root_directory_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(consistency_checker_root_directory_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME consistency_checker_root_directory_test
    COMMAND consistency_checker_root_directory_test
  )

  add_executable(consistency_checker_unreachable_inode_test
    tests/consistency_checker_unreachable_inode_test.cpp
  )
  target_link_libraries(consistency_checker_unreachable_inode_test PRIVATE
    eufs_consistency_checker eufs_writable_image eufs_image_reader
    eufs_mkfs_core
  )
  target_compile_options(consistency_checker_unreachable_inode_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(consistency_checker_unreachable_inode_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME consistency_checker_unreachable_inode_test
    COMMAND consistency_checker_unreachable_inode_test
  )

  add_executable(consistency_checker_directory_cycle_test
    tests/consistency_checker_directory_cycle_test.cpp
  )
  target_link_libraries(consistency_checker_directory_cycle_test PRIVATE
    eufs_consistency_checker eufs_writable_image eufs_image_reader
    eufs_mkfs_core
  )
  target_compile_options(consistency_checker_directory_cycle_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(consistency_checker_directory_cycle_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME consistency_checker_directory_cycle_test
    COMMAND consistency_checker_directory_cycle_test
  )

  add_executable(consistency_checker_block_reference_test
    tests/consistency_checker_block_reference_test.cpp
  )
  target_link_libraries(consistency_checker_block_reference_test PRIVATE
    eufs_consistency_checker eufs_writable_image eufs_image_reader
    eufs_mkfs_core
  )
  target_compile_options(consistency_checker_block_reference_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(consistency_checker_block_reference_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME consistency_checker_block_reference_test
    COMMAND consistency_checker_block_reference_test
  )

  add_executable(consistency_checker_bitmap_geometry_test
    tests/consistency_checker_bitmap_geometry_test.cpp
  )
  target_link_libraries(consistency_checker_bitmap_geometry_test PRIVATE
    eufsck_core eufs_mkfs_core
  )
  target_compile_options(consistency_checker_bitmap_geometry_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(consistency_checker_bitmap_geometry_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME consistency_checker_bitmap_geometry_test
    COMMAND consistency_checker_bitmap_geometry_test
  )

  add_executable(consistency_checker_link_count_test
    tests/consistency_checker_link_count_test.cpp
  )
  target_link_libraries(consistency_checker_link_count_test PRIVATE
    eufs_consistency_checker eufs_writable_image eufs_image_reader
    eufs_mkfs_core
  )
  target_compile_options(consistency_checker_link_count_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(consistency_checker_link_count_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME consistency_checker_link_count_test
    COMMAND consistency_checker_link_count_test
  )

  add_executable(eufsck_command_test tests/eufsck_command_test.cpp)
  target_link_libraries(eufsck_command_test PRIVATE
    eufsck_core eufs_mkfs_core
  )
  target_compile_options(eufsck_command_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(eufsck_command_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME eufsck_command_test COMMAND eufsck_command_test)

  add_executable(journal_control_image_test tests/journal_control_image_test.cpp)
  target_link_libraries(journal_control_image_test PRIVATE eufs_image_reader eufs_mkfs_core)
  target_compile_options(journal_control_image_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(journal_control_image_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME journal_control_image_test COMMAND journal_control_image_test)

  add_executable(journal_control_store_test tests/journal_control_store_test.cpp)
  target_link_libraries(journal_control_store_test PRIVATE eufs_journal_control_store eufs_image_reader eufs_mkfs_core)
  target_compile_options(journal_control_store_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(journal_control_store_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME journal_control_store_test COMMAND journal_control_store_test)

  add_executable(journal_body_store_test tests/journal_body_store_test.cpp)
  target_link_libraries(journal_body_store_test PRIVATE eufs_journal_control_store eufs_image_reader eufs_mkfs_core)
  target_compile_options(journal_body_store_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(journal_body_store_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME journal_body_store_test COMMAND journal_body_store_test)

  add_executable(ordered_transaction_stage_test tests/ordered_transaction_stage_test.cpp)
  target_link_libraries(ordered_transaction_stage_test PRIVATE eufs_journal_control_store eufs_first_block_write_plan eufs_writable_image eufs_image_reader eufs_mkfs_core)
  target_compile_options(ordered_transaction_stage_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(ordered_transaction_stage_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME ordered_transaction_stage_test COMMAND ordered_transaction_stage_test)

  add_executable(journal_commit_store_test tests/journal_commit_store_test.cpp)
  target_link_libraries(journal_commit_store_test PRIVATE eufs_journal_control_store eufs_mkfs_core)
  target_compile_options(journal_commit_store_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(journal_commit_store_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME journal_commit_store_test COMMAND journal_commit_store_test)

  add_executable(journal_recovery_classifier_test tests/journal_recovery_classifier_test.cpp)
  target_link_libraries(journal_recovery_classifier_test PRIVATE eufs_journal_control_store eufs_mkfs_core)
  target_compile_options(journal_recovery_classifier_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(journal_recovery_classifier_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME journal_recovery_classifier_test COMMAND journal_recovery_classifier_test)

  add_executable(journal_recovery_executor_test tests/journal_recovery_executor_test.cpp)
  target_link_libraries(journal_recovery_executor_test PRIVATE eufs_journal_control_store eufs_writable_image eufs_mkfs_core)
  target_compile_options(journal_recovery_executor_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(journal_recovery_executor_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME journal_recovery_executor_test COMMAND journal_recovery_executor_test)

  add_executable(ring_reservation_test tests/ring_reservation_test.cpp)
  target_link_libraries(ring_reservation_test PRIVATE eufs_ring_reservation)
  target_compile_options(ring_reservation_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME ring_reservation_test COMMAND ring_reservation_test)

  add_executable(bitmap_allocator_test tests/bitmap_allocator_test.cpp)
  target_link_libraries(bitmap_allocator_test PRIVATE eufs_bitmap_allocator Threads::Threads)
  target_compile_options(bitmap_allocator_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME bitmap_allocator_test COMMAND bitmap_allocator_test)

  add_executable(empty_file_create_plan_test tests/empty_file_create_plan_test.cpp)
  target_link_libraries(empty_file_create_plan_test PRIVATE eufs_create_plan eufs_mkfs_core)
  target_compile_options(empty_file_create_plan_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(empty_file_create_plan_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME empty_file_create_plan_test COMMAND empty_file_create_plan_test)

  add_executable(first_block_write_plan_test tests/first_block_write_plan_test.cpp)
  target_link_libraries(first_block_write_plan_test PRIVATE eufs_first_block_write_plan eufs_writable_image eufs_mkfs_core)
  target_compile_options(first_block_write_plan_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(first_block_write_plan_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME first_block_write_plan_test COMMAND first_block_write_plan_test)

  add_executable(file_write_plan_test tests/file_write_plan_test.cpp)
  target_link_libraries(file_write_plan_test PRIVATE
    eufs_file_write_plan eufs_writable_image eufs_create_plan
    eufs_image_reader eufs_mkfs_core
  )
  target_compile_options(file_write_plan_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(file_write_plan_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME file_write_plan_test COMMAND file_write_plan_test)

  add_executable(file_write_indirect_recovery_test
    tests/file_write_indirect_recovery_test.cpp
  )
  target_link_libraries(file_write_indirect_recovery_test PRIVATE
    eufs_file_write_plan eufs_writable_image eufs_create_plan
    eufs_journal_control_store eufs_consistency_checker eufs_image_reader
    eufs_mkfs_core
  )
  target_compile_options(file_write_indirect_recovery_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(file_write_indirect_recovery_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME file_write_indirect_recovery_test
    COMMAND file_write_indirect_recovery_test
  )

  add_executable(new_object_plan_test
    tests/new_object_plan_test.cpp
  )
  target_link_libraries(new_object_plan_test PRIVATE
    eufs_new_object_plan eufs_journal_control_store
    eufs_consistency_checker eufs_mkfs_core
  )
  target_compile_options(new_object_plan_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(new_object_plan_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME new_object_plan_test COMMAND new_object_plan_test)

  add_executable(object_replace_plan_test
    tests/object_replace_plan_test.cpp
  )
  target_link_libraries(object_replace_plan_test PRIVATE
    eufs_object_replace_plan eufs_file_write_plan eufs_new_object_plan
    eufs_journal_control_store eufs_consistency_checker eufs_mkfs_core
  )
  target_compile_options(object_replace_plan_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(object_replace_plan_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME object_replace_plan_test COMMAND object_replace_plan_test)

  add_executable(object_replace_recovery_test
    tests/object_replace_recovery_test.cpp
  )
  target_link_libraries(object_replace_recovery_test PRIVATE
    eufs_object_replace_plan eufs_new_object_plan eufs_journal_control_store
    eufs_consistency_checker eufs_mkfs_core
  )
  target_compile_options(object_replace_recovery_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(object_replace_recovery_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME object_replace_recovery_test COMMAND object_replace_recovery_test)

  add_executable(new_object_recovery_test
    tests/new_object_recovery_test.cpp
  )
  target_link_libraries(new_object_recovery_test PRIVATE
    eufs_new_object_plan eufs_journal_control_store
    eufs_consistency_checker eufs_mkfs_core
  )
  target_compile_options(new_object_recovery_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(new_object_recovery_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME new_object_recovery_test COMMAND new_object_recovery_test)

  add_executable(object_backend_test
    tests/object_backend_test.cpp
  )
  target_link_libraries(object_backend_test PRIVATE
    eufs_object_backend eufs_consistency_checker eufs_mkfs_core
    Threads::Threads
  )
  target_compile_options(object_backend_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(object_backend_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME object_backend_test COMMAND object_backend_test)

  add_executable(rpc_admission_queue_test
    tests/rpc_admission_queue_test.cpp
  )
  target_link_libraries(rpc_admission_queue_test PRIVATE
    eufs_rpc_admission Threads::Threads
  )
  target_compile_options(rpc_admission_queue_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  add_test(NAME rpc_admission_queue_test COMMAND rpc_admission_queue_test)

  if(EUFS_BUILD_BRPC_SERVICE)
    add_executable(object_service_metrics_test
      tests/object_service_metrics_test.cpp
    )
    target_link_libraries(object_service_metrics_test PRIVATE
      eufs_object_service
    )
    target_compile_options(object_service_metrics_test PRIVATE
      -Wall -Wextra -Wpedantic
    )
    add_test(NAME object_service_metrics_test
      COMMAND object_service_metrics_test
    )
  endif()

  add_executable(request_fingerprint_test
    tests/request_fingerprint_test.cpp
  )
  target_link_libraries(request_fingerprint_test PRIVATE
    eufs_request_fingerprint
  )
  target_compile_options(request_fingerprint_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  add_test(NAME request_fingerprint_test COMMAND request_fingerprint_test)

  add_executable(request_ledger_format_test
    tests/request_ledger_format_test.cpp
  )
  target_link_libraries(request_ledger_format_test PRIVATE
    eufs_request_ledger_format
  )
  target_compile_options(request_ledger_format_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  add_test(NAME request_ledger_format_test COMMAND request_ledger_format_test)

  add_executable(request_ledger_index_test
    tests/request_ledger_index_test.cpp
  )
  target_link_libraries(request_ledger_index_test PRIVATE
    eufs_request_ledger_index eufs_object_backend eufs_consistency_checker
    eufs_mkfs_core
  )
  target_compile_options(request_ledger_index_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(request_ledger_index_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME request_ledger_index_test COMMAND request_ledger_index_test)

  add_executable(request_ledger_plan_test
    tests/request_ledger_plan_test.cpp
  )
  target_link_libraries(request_ledger_plan_test PRIVATE
    eufs_request_ledger_plan eufs_mkfs_core
  )
  target_compile_options(request_ledger_plan_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(request_ledger_plan_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME request_ledger_plan_test COMMAND request_ledger_plan_test)

  add_executable(request_ledger_transaction_recovery_test
    tests/request_ledger_transaction_recovery_test.cpp
  )
  target_link_libraries(request_ledger_transaction_recovery_test PRIVATE
    eufs_request_ledger_plan eufs_new_object_plan
    eufs_journal_transaction_executor eufs_consistency_checker eufs_mkfs_core
  )
  target_compile_options(request_ledger_transaction_recovery_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(request_ledger_transaction_recovery_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME request_ledger_transaction_recovery_test
    COMMAND request_ledger_transaction_recovery_test
  )

  add_executable(object_backend_idempotency_test
    tests/object_backend_idempotency_test.cpp
  )
  target_link_libraries(object_backend_idempotency_test PRIVATE
    eufs_object_backend eufs_request_fingerprint eufs_mkfs_core
  )
  target_compile_options(object_backend_idempotency_test PRIVATE
    -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(object_backend_idempotency_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME object_backend_idempotency_test
    COMMAND object_backend_idempotency_test
  )

  add_executable(first_block_write_apply_test tests/first_block_write_apply_test.cpp)
  target_link_libraries(first_block_write_apply_test PRIVATE eufs_writable_image eufs_mkfs_core eufs_image_reader)
  target_compile_options(first_block_write_apply_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(first_block_write_apply_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME first_block_write_apply_test COMMAND first_block_write_apply_test)

  add_executable(writable_image_test tests/writable_image_test.cpp)
  target_link_libraries(writable_image_test PRIVATE eufs_writable_image eufs_mkfs_core eufs_image_reader)
  target_compile_options(writable_image_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(writable_image_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME writable_image_test COMMAND writable_image_test)

  add_executable(mounted_image_session_test tests/mounted_image_session_test.cpp)
  target_link_libraries(mounted_image_session_test PRIVATE eufs_mounted_image_session)
  target_compile_options(mounted_image_session_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME mounted_image_session_test COMMAND mounted_image_session_test)

  add_executable(mounted_image_store_adoption_test tests/mounted_image_store_adoption_test.cpp)
  target_link_libraries(mounted_image_store_adoption_test PRIVATE eufs_mounted_image_session eufs_journal_control_store eufs_mkfs_core)
  target_compile_options(mounted_image_store_adoption_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(mounted_image_store_adoption_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME mounted_image_store_adoption_test COMMAND mounted_image_store_adoption_test)

  add_executable(mounted_image_reader_adoption_test tests/mounted_image_reader_adoption_test.cpp)
  target_link_libraries(mounted_image_reader_adoption_test PRIVATE eufs_mounted_image_session eufs_image_reader eufs_mkfs_core)
  target_compile_options(mounted_image_reader_adoption_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(mounted_image_reader_adoption_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME mounted_image_reader_adoption_test COMMAND mounted_image_reader_adoption_test)

  add_executable(mounted_image_mutation_adoption_test tests/mounted_image_mutation_adoption_test.cpp)
  target_link_libraries(mounted_image_mutation_adoption_test PRIVATE eufs_mounted_image_session eufs_writable_image eufs_image_reader eufs_mkfs_core)
  target_compile_options(mounted_image_mutation_adoption_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(mounted_image_mutation_adoption_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME mounted_image_mutation_adoption_test COMMAND mounted_image_mutation_adoption_test)

  add_executable(mount_state_startup_test tests/mount_state_startup_test.cpp)
  target_link_libraries(mount_state_startup_test PRIVATE eufs_mount_state eufs_mkfs_core)
  target_compile_options(mount_state_startup_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(mount_state_startup_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME mount_state_startup_test COMMAND mount_state_startup_test)

  add_executable(fuse_write_error_contract_test
    tests/fuse_write_error_contract_test.cpp
    src/fuse/operations.cpp
  )
  target_include_directories(fuse_write_error_contract_test PRIVATE
    ${FUSE3_INCLUDE_DIRS}
  )
  target_link_libraries(fuse_write_error_contract_test PRIVATE
    eufs_mount_state eufs_writable_image eufs_consistency_checker
    eufs_mkfs_core eufs_journal_transaction_executor
  )
  target_compile_options(fuse_write_error_contract_test PRIVATE
    ${FUSE3_CFLAGS_OTHER} -Wall -Wextra -Wpedantic
  )
  target_compile_definitions(fuse_write_error_contract_test PRIVATE
    _FILE_OFFSET_BITS=64
  )
  add_test(NAME fuse_write_error_contract_test
    COMMAND fuse_write_error_contract_test
  )
endif()
