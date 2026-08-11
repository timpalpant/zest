cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zest_smoke)
target_sources(app PRIVATE src/main.cpp)

if(CONFIG_ZEST_JSON OR CONFIG_ZEST_CBOR)
  target_sources(app PRIVATE src/serde.cpp)
endif()
