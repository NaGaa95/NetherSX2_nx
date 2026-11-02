cmake_minimum_required(VERSION 3.22)
project(NetherSX2NetworkValidation LANGUAGES C)

add_executable(network_validation validation.c ../../source/network.c)
target_compile_features(network_validation PRIVATE c_std_11)

enable_testing()
add_test(NAME network_validation COMMAND network_validation)
set_tests_properties(network_validation PROPERTIES TIMEOUT 15)
