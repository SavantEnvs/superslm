# Paged-KV plan (rev 16.1): the red suite's per-step executables (step C1).
#
# One executable per owning step, so each links once its own step's builder has defined every
# symbol its cells call, and not before (red by link, tests/paged-kv/pkv_abi.h):
#   superslm_pkv_r0   the harness's own checks; links and runs on the base
#   superslm_pkv_c2   engine cells (the page view, per-page attention, GemmProbQ15AccumulateInto)
#   superslm_pkv_c3   page-module cells
#   superslm_pkv_c4   legacy-holder ABI cells and the C4 seams
#   superslm_pkv_c5   budget-mode ABI cells
#   superslm_pkv_c6   the box's achievement and timing cells (real artifacts)
# Every one is EXCLUDE_FROM_ALL until its step turns it green, so the default build stays green;
# tools/check_paged_kv_red.sh builds each and confirms its link fails on exactly the symbols its
# step owes. Cells read SUPERSLM_PAGED_KV_FIXTURE_DIR (tools/gen_paged_kv_fixture.py).

set(PKV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests/paged-kv)

function(superslm_pkv_target name)
	cmake_parse_arguments(PKV "" "LIBRARY" "SOURCES" ${ARGN})
	if(NOT PKV_LIBRARY)
		set(PKV_LIBRARY superslm_test_injection)
	endif()
	add_executable(${name} EXCLUDE_FROM_ALL ${PKV_DIR}/pkv_main.cpp ${PKV_SOURCES})
	target_link_libraries(${name} PRIVATE ${PKV_LIBRARY})
	target_include_directories(${name} PRIVATE ${PKV_DIR} include src tests)
	target_compile_definitions(${name} PRIVATE PKV_REFERENCE_DIR="${PKV_DIR}/reference")
	if(MSVC)
		target_compile_options(${name} PRIVATE /W4 /fp:precise)
	else()
		target_compile_options(${name} PRIVATE -Wall -Wextra -ffp-contract=off)
		find_package(Threads REQUIRED)
		target_link_libraries(${name} PRIVATE Threads::Threads)
	endif()
endfunction()

superslm_pkv_target(superslm_pkv_r0 SOURCES ${PKV_DIR}/r0_harness_selftest.cpp)

file(GLOB PKV_C2_SOURCES CONFIGURE_DEPENDS ${PKV_DIR}/c2_*.cpp)
file(GLOB PKV_C3_SOURCES CONFIGURE_DEPENDS ${PKV_DIR}/c3_*.cpp)
file(GLOB PKV_C4_SOURCES CONFIGURE_DEPENDS ${PKV_DIR}/c4_*.cpp)
file(GLOB PKV_C5_SOURCES CONFIGURE_DEPENDS ${PKV_DIR}/c5_*.cpp)
file(GLOB PKV_C6_SOURCES CONFIGURE_DEPENDS ${PKV_DIR}/c6_*.cpp)
foreach(step c2 c3 c4 c5 c6)
	string(TOUPPER ${step} STEP)
	if(PKV_${STEP}_SOURCES)
		superslm_pkv_target(superslm_pkv_${step} SOURCES ${PKV_${STEP}_SOURCES})
	endif()
endforeach()

# Red-first timer and CPUID decision-path constructions. Neither target is a
# production timing leg or eligible to publish a C6 reading.
superslm_pkv_target(superslm_pkv_74_no_tsc SOURCES ${PKV_DIR}/c6_dim7_reset_adopt_timing.cpp)
target_compile_definitions(superslm_pkv_74_no_tsc PRIVATE PKV_74_FORCE_NO_TSC=1)
superslm_pkv_target(superslm_pkv_74_no_subtraction SOURCES ${PKV_DIR}/c6_dim7_reset_adopt_timing.cpp)
target_compile_definitions(superslm_pkv_74_no_subtraction PRIVATE PKV_74_NO_SUBTRACTION_MUTANT=1)

# C6's measured 7.9/7.9b arms and piece-wise SSB5 prefill use the production
# library. This target has no test-injection compile definitions or link input.
add_executable(superslm_pkv_pair EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/tools/paged_kv_pair.cpp)
target_link_libraries(superslm_pkv_pair PRIVATE superslm)
target_include_directories(superslm_pkv_pair PRIVATE ${PKV_DIR} include src tests)
target_compile_definitions(superslm_pkv_pair PRIVATE PKV_REFERENCE_DIR="${PKV_DIR}/reference")
if(MSVC)
	target_compile_options(superslm_pkv_pair PRIVATE /W4 /fp:precise)
else()
	target_compile_options(superslm_pkv_pair PRIVATE -Wall -Wextra -ffp-contract=off)
endif()

# Lifecycle cost has a frozen v1.11.0 baseline and a paged leg linked to the
# production library. Its result cannot carry a seam or injection definition.
superslm_pkv_target(superslm_pkv_lifecycle_prod LIBRARY superslm SOURCES ${PKV_DIR}/c6_lifecycle_timing.cpp)
target_compile_definitions(superslm_pkv_lifecycle_prod PRIVATE PKV_LIFECYCLE_PRODUCTION_LEG=1)

# Link-time source-set receipts are build artifacts. The box runners re-hash
# these files and the invoked binaries/scripts before accepting commissioning.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(pkv_source_manifest target library)
	set(args --repo ${CMAKE_CURRENT_SOURCE_DIR} --exe $<TARGET_FILE:${target}>
		--library $<TARGET_FILE:${library}>)
	foreach(source IN LISTS ARGN)
		list(APPEND args --source ${source})
	endforeach()
	if(target STREQUAL "superslm_pkv_pair")
		list(APPEND args --invoked ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_paged_kv_pairs.py
			--invoked ${CMAKE_CURRENT_SOURCE_DIR}/tools/paged_kv_load.py)
	endif()
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/write_paged_kv_source_manifest.py ${args}
		VERBATIM)
endfunction()
set(PKV_DECIDING_COMMON ${PKV_DIR}/pkv_main.cpp ${PKV_DIR}/pkv_common.h
	${PKV_DIR}/pkv_budget_c_helpers.h ${PKV_DIR}/pkv_engine_api.h)
pkv_source_manifest(superslm_pkv_c6 superslm_test_injection ${PKV_C6_SOURCES}
	${PKV_DECIDING_COMMON} ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_paged_kv_c6_box.ps1
	${CMAKE_CURRENT_SOURCE_DIR}/tools/paged_kv_c6_provenance.ps1)
pkv_source_manifest(superslm_pkv_lifecycle_prod superslm ${PKV_DIR}/c6_lifecycle_timing.cpp
	${PKV_DECIDING_COMMON} ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_paged_kv_c6_box.ps1
	${CMAKE_CURRENT_SOURCE_DIR}/tools/paged_kv_c6_provenance.ps1)
pkv_source_manifest(superslm_pkv_pair superslm ${CMAKE_CURRENT_SOURCE_DIR}/tools/paged_kv_pair.cpp
	${PKV_DIR}/pkv_common.h ${PKV_DIR}/pkv_budget_c_helpers.h
	${CMAKE_CURRENT_SOURCE_DIR}/tools/run_paged_kv_pairs.py ${CMAKE_CURRENT_SOURCE_DIR}/tools/paged_kv_load.py)
get_target_property(PKV_PROD_DEFS superslm COMPILE_DEFINITIONS)
if("${PKV_PROD_DEFS}" MATCHES "SUPERSLM_.*(SEAM|INJECTION|INSTRUMENT|BENCH)")
	message(FATAL_ERROR "production superslm library carries a seam/injection definition; lifecycle grading refused")
endif()

# Cells 4.7/C5 and 6.3/C5: the DotRow tiers. superslm_pkv_c5's sources linked against each forced-tier
# library of CMakeLists.txt in place of superslm_test_injection (x86-64 only, like those libraries;
# EXCLUDE_FROM_ALL both). Each forced library carries the bad-alloc and matvec seams, so the whole C5
# suite links; a tier leg runs the two tier cells by id, e.g.
#   superslm_pkv_c5_avx2_forced 4.7/C5 6.3/C5
# which runs exactly those two cells; an argument that matches no registered id fails the run
# (pkv_main.cpp), so a renamed cell cannot drop out of a tier leg unnoticed.
# The table-sentinel trap (7.13) is compiled only into superslm_test_injection, so 7.13 belongs to the
# default binary, not to these.
if(PKV_C5_SOURCES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64|X64)$")
	foreach(tier sse2 avx2 avx512)
		superslm_pkv_target(superslm_pkv_c5_${tier}_forced LIBRARY superslm_${tier}_forced SOURCES ${PKV_C5_SOURCES})
	endforeach()
endif()

# Paged-KV plan rev 16.21 §23.8, step T0: the R9 red suite for steps T1 to T3 (the page-size rule and
# rotation, the partial last page, the contiguous pool option). Cells are tagged with their R9 step
# (T1, T2, T3) and their §7 id. Two executables:
#   superslm_pkv_r9      every bench cell: generated and CFG1-patched fixtures, no real artifact
#   superslm_pkv_r9_box  the real-artifact cells (SUPERSLM_PAGED_KV_REAL_ARTIFACT_DIR); authored and
#                        compiled at T0, run on the box only
# Both are EXCLUDE_FROM_ALL. The R9 verbs and the pin seam are declared in tests/paged-kv/pkv_r9.h.
# Until the builder defines them, pkv_r9_stubs.cpp defines each as an unimplemented stub (a refusal,
# nothing written), so the cells link and fail at their assertions. The builder turns each option
# OFF in the commit that defines the real symbol; a stub left ON beside a real definition fails the
# link with a duplicate symbol, so the two cannot coexist.
option(SUPERSLM_PKV_R9_STUB_CONTIGUOUS "T0 stubs for sslm_kv_pool_contiguous_size and sslm_kv_pool_create_contiguous" ON)
option(SUPERSLM_PKV_R9_STUB_PIN "T0 stub for the test-only pin KvPagesTestPinPagePositions" ON)
file(GLOB PKV_R9_SOURCES CONFIGURE_DEPENDS ${PKV_DIR}/r9_*.cpp)
file(GLOB PKV_R9_BOX_SOURCES CONFIGURE_DEPENDS ${PKV_DIR}/r9box_*.cpp)
foreach(r9 superslm_pkv_r9 superslm_pkv_r9_box)
	if(r9 STREQUAL "superslm_pkv_r9")
		superslm_pkv_target(${r9} SOURCES ${PKV_R9_SOURCES} ${PKV_DIR}/pkv_r9_stubs.cpp)
	else()
		superslm_pkv_target(${r9} SOURCES ${PKV_R9_BOX_SOURCES} ${PKV_DIR}/pkv_r9_stubs.cpp)
	endif()
	if(SUPERSLM_PKV_R9_STUB_CONTIGUOUS)
		target_compile_definitions(${r9} PRIVATE PKV_R9_STUB_CONTIGUOUS=1)
	endif()
	if(SUPERSLM_PKV_R9_STUB_PIN)
		target_compile_definitions(${r9} PRIVATE PKV_R9_STUB_PIN=1)
	endif()
endforeach()

# Cell 4.12's link half: a unit that references the pin, linked to the production `superslm`
# library, must FAIL to link (tests/paged-kv/r9_pin_reach.py builds this target and expects a link
# error naming the pin). It never carries the stub.
add_executable(superslm_pkv_r9_pin_link_probe EXCLUDE_FROM_ALL ${PKV_DIR}/pkv_r9_pin_link_probe.cpp)
target_link_libraries(superslm_pkv_r9_pin_link_probe PRIVATE superslm)
target_include_directories(superslm_pkv_r9_pin_link_probe PRIVATE ${PKV_DIR} include)
