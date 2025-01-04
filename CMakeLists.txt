cmake_minimum_required(VERSION 3.20)
project(ServoControl)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_CMAKE_DIR}"
        CACHE STRING "cmake toolchain file")
list(APPEND CMAKE_PREFIX_PATH
        "$ENV{VCPKG_ROOT}/installed/x64-windows"
)

# Set Npcap SDK path
set(NPCAP_SDK_DIR "E:/00.CODES/PACKAGES/npcap")

# Set these variables explicitly for FindPacket.cmake
set(Packet_ROOT "${NPCAP_SDK_DIR}")
set(Packet_INCLUDE_DIR "${NPCAP_SDK_DIR}/Include")
set(Packet_LIBRARY "${NPCAP_SDK_DIR}/Lib/x64/Packet.lib")

# Set these variables explicitly for FindPCAP.cmake
set(PCAP_ROOT "${NPCAP_SDK_DIR}")
set(PCAP_INCLUDE_DIR "${NPCAP_SDK_DIR}/Include")
set(PCAP_LIBRARY "${NPCAP_SDK_DIR}/Lib/x64/wpcap.lib")

# Create imported targets manually
if (EXISTS "${Packet_LIBRARY}" AND EXISTS "${PCAP_LIBRARY}")
    if (NOT TARGET Packet::Packet)
        add_library(Packet::Packet STATIC IMPORTED)
        set_target_properties(Packet::Packet PROPERTIES
                IMPORTED_LOCATION "${Packet_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${Packet_INCLUDE_DIR}")
    endif ()

    if (NOT TARGET PCAP::PCAP)
        add_library(PCAP::PCAP STATIC IMPORTED)
        set_target_properties(PCAP::PCAP PROPERTIES
                IMPORTED_LOCATION "${PCAP_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${PCAP_INCLUDE_DIR}")
    endif ()
endif ()

# Find PcapPlusPlus
find_package(PcapPlusPlus CONFIG REQUIRED)

add_executable(ServoControl main.cpp)

target_include_directories(ServoControl PRIVATE
        ${Packet_INCLUDE_DIR}
        ${PCAP_INCLUDE_DIR}
)

target_link_libraries(ServoControl PRIVATE
        Packet::Packet
        PCAP::PCAP
        PcapPlusPlus::Pcap++
        PcapPlusPlus::Common++
)