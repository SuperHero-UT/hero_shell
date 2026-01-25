set(_SUPERHERO_PROTO_TARGET "${PROJECT_NAME}_proto")

add_library(
  ${_SUPERHERO_PROTO_TARGET} ${superhero_grpc_srcs} ${superhero_grpc_hdrs}
                             ${superhero_proto_srcs} ${superhero_proto_hdrs})

set(superhero_proto_files
    "${CMAKE_CURRENT_SOURCE_DIR}/superhero.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/hlcommands.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/rmap.proto")
set(superhero_proto_path ${CMAKE_CURRENT_SOURCE_DIR})
set(superhero_proto_srcs "")
set(superhero_proto_hdrs "")
set(superhero_grpc_srcs "")
set(superhero_grpc_hdrs "")

foreach(proto ${superhero_proto_files})
  get_filename_component(proto_name ${proto} NAME_WE)
  list(APPEND superhero_proto_srcs
       "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.pb.cc")
  list(APPEND superhero_proto_hdrs
       "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.pb.h")
  list(APPEND superhero_grpc_srcs
       "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.grpc.pb.cc")
  list(APPEND superhero_grpc_hdrs
       "${CMAKE_CURRENT_BINARY_DIR}/${proto_name}.grpc.pb.h")
endforeach()

add_custom_command(
  OUTPUT ${superhero_proto_srcs} ${superhero_proto_hdrs} ${superhero_grpc_srcs}
         ${superhero_grpc_hdrs}
  COMMAND
    ${SUPERHERO_PROTOC} ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}" --cpp_out
    "${CMAKE_CURRENT_BINARY_DIR}" -I "${superhero_proto_path}"
    --plugin=protoc-gen-grpc="${SUPERHERO_GRPC_CPP_PLUGIN}"
    ${superhero_proto_files}
  DEPENDS ${superhero_proto_files} protobuf::protoc grpc_cpp_plugin)

target_include_directories(${_SUPERHERO_PROTO_TARGET} SYSTEM
                           PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_sources(${_SUPERHERO_PROTO_TARGET} PRIVATE ${superhero_proto_srcs}
                                                  ${superhero_grpc_srcs})
target_link_libraries(${_SUPERHERO_PROTO_TARGET} PUBLIC grpc++_unsecure)
