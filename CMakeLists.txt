cmake_minimum_required(VERSION 3.16)
project(eufs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)
find_package(Threads REQUIRED)

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
target_link_libraries(eufs_consistency_checker PUBLIC eufs_ondisk)
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

add_library(eufs_first_block_write_plan
  src/metadata/first_block_write_plan.cpp
)
target_include_directories(eufs_first_block_write_plan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_first_block_write_plan PUBLIC eufs_image_reader eufs_bitmap_allocator eufs_ondisk)
target_compile_options(eufs_first_block_write_plan PRIVATE -Wall -Wextra -Wpedantic)

add_library(eufs_file_write_plan
  src/metadata/file_write_plan.cpp
)
target_include_directories(eufs_file_write_plan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_file_write_plan PUBLIC eufs_image_reader eufs_bitmap_allocator eufs_ondisk)
target_compile_options(eufs_file_write_plan PRIVATE -Wall -Wextra -Wpedantic)

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
  src/journal/journal_control_store.cpp
  src/journal/journal_recovery.cpp
)
target_include_directories(eufs_journal_control_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_journal_control_store PUBLIC eufs_ring_reservation)
target_compile_options(eufs_journal_control_store PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(eufs_journal_control_store PRIVATE _FILE_OFFSET_BITS=64)

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

add_library(eufs_writable_image
  src/storage/writable_image.cpp
)
target_include_directories(eufs_writable_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_writable_image PUBLIC eufs_create_plan eufs_first_block_write_plan eufs_file_write_plan eufs_ondisk)
target_compile_options(eufs_writable_image PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(eufs_writable_image PRIVATE _FILE_OFFSET_BITS=64)

add_library(eufs_stage_c_state
  src/fuse/stage_c_state.cpp
)
target_include_directories(eufs_stage_c_state PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(eufs_stage_c_state PUBLIC eufs_mounted_image_session eufs_journal_control_store eufs_image_reader)
target_compile_options(eufs_stage_c_state PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(eufs_stage_c_state PRIVATE _FILE_OFFSET_BITS=64)

add_executable(eufs_mkfs src/app/mkfs_main.cpp)
set_target_properties(eufs_mkfs PROPERTIES OUTPUT_NAME "eufs-mkfs")
target_link_libraries(eufs_mkfs PRIVATE eufs_mkfs_core)
target_compile_options(eufs_mkfs PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(eufs_mkfs PRIVATE _FILE_OFFSET_BITS=64)

add_executable(eufsd
  src/app/eufsd_main.cpp
  src/fuse/read_only_operations.cpp
)
target_include_directories(eufsd PRIVATE ${FUSE3_INCLUDE_DIRS})
target_link_directories(eufsd PRIVATE ${FUSE3_LIBRARY_DIRS})
target_link_libraries(eufsd PRIVATE eufs_stage_c_state eufs_writable_image ${FUSE3_LIBRARIES})
target_compile_options(eufsd PRIVATE ${FUSE3_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)
target_compile_definitions(eufsd PRIVATE _FILE_OFFSET_BITS=64)

add_executable(eufs src/main.cpp)
target_include_directories(eufs PRIVATE ${FUSE3_INCLUDE_DIRS})
target_link_directories(eufs PRIVATE ${FUSE3_LIBRARY_DIRS})
target_link_libraries(eufs PRIVATE ${FUSE3_LIBRARIES})
target_compile_options(eufs PRIVATE ${FUSE3_CFLAGS_OTHER} -Wall -Wextra -Wpedantic)
target_compile_definitions(eufs PRIVATE _FILE_OFFSET_BITS=64)

include(CTest)
if(BUILD_TESTING)
  enable_testing()
  add_executable(ondisk_format_test tests/ondisk_format_test.cpp)
  target_link_libraries(ondisk_format_test PRIVATE eufs_ondisk)
  target_compile_options(ondisk_format_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME ondisk_format_test COMMAND ondisk_format_test)

  add_executable(ondisk_journal_test tests/ondisk_journal_test.cpp)
  target_link_libraries(ondisk_journal_test PRIVATE eufs_journal_format)
  target_compile_options(ondisk_journal_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME ondisk_journal_test COMMAND ondisk_journal_test)

  add_executable(mkfs_image_test tests/mkfs_image_test.cpp)
  target_link_libraries(mkfs_image_test PRIVATE eufs_mkfs_core)
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

  add_executable(stage_c_state_startup_test tests/stage_c_state_startup_test.cpp)
  target_link_libraries(stage_c_state_startup_test PRIVATE eufs_stage_c_state eufs_mkfs_core)
  target_compile_options(stage_c_state_startup_test PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(stage_c_state_startup_test PRIVATE _FILE_OFFSET_BITS=64)
  add_test(NAME stage_c_state_startup_test COMMAND stage_c_state_startup_test)
endif()
