# broker_exec::adapters — the ONLY layer permitted to import broker SDKs and
# transports (REST/WS clients). Real adapters (Kite, Kotak, fake broker) attach
# their sources here in later epics. Header-only INTERFACE target until then.

add_library(broker_exec_adapters INTERFACE)
add_library(broker_exec::adapters ALIAS broker_exec_adapters)

target_include_directories(broker_exec_adapters
  INTERFACE ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(broker_exec_adapters
  INTERFACE
    broker_exec_ports
    broker_exec_domain)
