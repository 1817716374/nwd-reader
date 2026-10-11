# Private, symbol-prefixed Expat; no dependency headers in the public API.
include(TestBigEndian)
test_big_endian(NWD_XML_BIG_ENDIAN)
if(NWD_XML_BIG_ENDIAN)
  set(NWD_XML_BYTEORDER 4321)
else()
  set(NWD_XML_BYTEORDER 1234)
endif()
configure_file(${CMAKE_CURRENT_LIST_DIR}/expat_config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/expat/expat_config.h @ONLY)
add_library(nwd_expat STATIC
  third_party/expat/lib/xmlparse.c
  third_party/expat/lib/xmlrole.c
  third_party/expat/lib/xmltok.c)
if(WIN32)
  target_sources(nwd_expat PRIVATE third_party/expat/lib/random_rand_s.c)
else()
  target_sources(nwd_expat PRIVATE third_party/expat/lib/random_dev_urandom.c)
endif()
set_target_properties(nwd_expat PROPERTIES EXPORT_NAME expat C_STANDARD 99)
target_include_directories(nwd_expat PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/expat
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/expat)
target_compile_definitions(nwd_expat PRIVATE XML_STATIC XML_ATTR_INFO)
if(MSVC)
  target_compile_options(nwd_expat PRIVATE
    /FI${CMAKE_CURRENT_SOURCE_DIR}/third_party/expat/nwd_expat_prefix.h)
else()
  target_compile_options(nwd_expat PRIVATE
    -include ${CMAKE_CURRENT_SOURCE_DIR}/third_party/expat/nwd_expat_prefix.h)
endif()
