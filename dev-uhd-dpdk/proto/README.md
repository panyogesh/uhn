protoc -I. --cpp_out=../src/generated_proto/ --grpc_out=../src/generated_proto/ --plugin=protoc-gen-grpc=`which grpc_cpp_plugin` ./flexsdr.proto
