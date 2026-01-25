#pragma once

#include <superhero.grpc.pb.h>
#include <superhero.pb.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace superhero::grpc {

namespace {
constexpr auto kRmapResultPollInterval = std::chrono::milliseconds(15);
constexpr int kRmapResultMaxAttempts = 100;
}  // namespace

inline auto echo(superhero::CommunicationService::Stub& stub, const std::string& message)
    -> std::string {
  superhero::EchoRequest request;
  request.set_message(message);

  superhero::EchoReply reply;
  ::grpc::ClientContext context;

  ::grpc::Status status = stub.Echo(&context, request, &reply);

  if (!status.ok()) {
    throw std::runtime_error("gRPC Echo failed: " + status.error_message());
  }
  return reply.message();
}

inline auto rmapRead(superhero::CommunicationService::Stub& stub, uint16_t logical_address,
                     ::superhero::CdTeDSDAddress address, uint32_t length) -> std::vector<uint8_t> {
  auto task_id = 0;
  {
    superhero::RmapReadSubmitRequest request;
    superhero::RmapSubmitReply reply;

    request.set_logical_address(logical_address);
    request.set_address(address);
    request.set_length(length);

    ::grpc::ClientContext context;
    ::grpc::Status status = stub.RmapReadSubmit(&context, request, &reply);
    if (!status.ok()) {
      throw std::runtime_error("gRPC RmapSubmit (Read) failed: " + status.error_message());
    }

    switch (reply.status()) {
      case superhero::RmapSubmitStatus::RmapSubmit_Accepted:
        task_id = reply.task_id();
        break;
      case superhero::RmapSubmitStatus::RmapSubmit_InvalidArgument:
        throw std::runtime_error("RmapSubmit (Read) rejected: Invalid Argument\n" +
                                 reply.message());
      case superhero::RmapSubmitStatus::RmapSubmit_Denied:
        throw std::runtime_error("RmapSubmit (Read) rejected: Denied\n" + reply.message());
      case superhero::RmapSubmitStatus::RmapSubmit_TaskQueueIsFull:
        throw std::runtime_error("RmapSubmit (Read) rejected: Task Queue Is Full\n" +
                                 reply.message());
      default:
        throw std::runtime_error("RmapSubmit (Read) returned unknown status: " +
                                 std::to_string(static_cast<int>(reply.status())) + "\n" +
                                 reply.message());
    }
  }
  {
    for (int i = 0; i < kRmapResultMaxAttempts; ++i) {
      std::this_thread::sleep_for(kRmapResultPollInterval);
      superhero::RmapResultRequest request;
      superhero::RmapResultReply reply;

      request.set_task_id(task_id);

      ::grpc::ClientContext context;
      ::grpc::Status status = stub.RmapResult(&context, request, &reply);
      if (!status.ok()) {
        throw std::runtime_error("gRPC RmapResult (Read) failed: " + status.error_message());
      }
      switch (reply.status()) {
        case superhero::RmapResultStatus::RmapResult_Ok: {
          std::vector<uint8_t> data(reply.value().begin(), reply.value().end());
          return data;
        }
        case superhero::RmapResultStatus::RmapResult_Error:
          throw std::runtime_error("RmapResult (Read): Result Error\n" + reply.message());
        case superhero::RmapResultStatus::RmapResult_Waiting:
          std::cerr << "RmapResult (Read): Waiting\n";
          continue;
        default:
          throw std::runtime_error("RmapResult (Read) returned unknown status: " +
                                   std::to_string(static_cast<int>(reply.status())) + "\n" +
                                   reply.message());
      }
    }
  }
  throw std::runtime_error("RmapResult (Read) timed out without a response");
}

inline auto rmapWrite(superhero::CommunicationService::Stub& stub, uint16_t logical_address,
                      ::superhero::CdTeDSDAddress address, const std::vector<uint8_t>& data)
    -> bool {
  int32_t task_id = 0;
  {
    superhero::RmapWriteSubmitRequest request;
    superhero::RmapSubmitReply reply;

    request.set_logical_address(logical_address);
    request.set_address(address);
    request.set_value(data.data(), data.size());

    ::grpc::ClientContext context;
    ::grpc::Status status = stub.RmapWriteSubmit(&context, request, &reply);

    if (!status.ok()) {
      throw std::runtime_error("gRPC RmapSubmit (Write) failed: " + status.error_message());
    }

    switch (reply.status()) {
      case superhero::RmapSubmitStatus::RmapSubmit_Accepted:
        task_id = reply.task_id();
        break;
      case superhero::RmapSubmitStatus::RmapSubmit_InvalidArgument:
        throw std::runtime_error("RmapSubmit (Write) rejected: Invalid Argument\n" +
                                 reply.message());
      case superhero::RmapSubmitStatus::RmapSubmit_Denied:
        throw std::runtime_error("RmapSubmit (Write) rejected: Denied\n" + reply.message());
      case superhero::RmapSubmitStatus::RmapSubmit_TaskQueueIsFull:
        throw std::runtime_error("RmapSubmit (Write) rejected: Task Queue Is Full\n" +
                                 reply.message());
      default:
        throw std::runtime_error("RmapSubmit (Write) returned unknown status: " +
                                 std::to_string(static_cast<int>(reply.status())) + "\n" +
                                 reply.message());
    }
  }
  {
    for (int i = 0; i < kRmapResultMaxAttempts; ++i) {
      std::this_thread::sleep_for(kRmapResultPollInterval);
      superhero::RmapResultRequest request;
      superhero::RmapResultReply reply;

      request.set_task_id(task_id);

      ::grpc::ClientContext context;
      ::grpc::Status status = stub.RmapResult(&context, request, &reply);

      if (!status.ok()) {
        throw std::runtime_error("gRPC RmapResult (Write) failed: " + status.error_message());
      }
      switch (reply.status()) {
        case superhero::RmapResultStatus::RmapResult_Ok:
          return true;
        case superhero::RmapResultStatus::RmapResult_Error:
          throw std::runtime_error("RmapResult (Write): Result Error\n" + reply.message());
        case superhero::RmapResultStatus::RmapResult_Waiting:
          std::cerr << "RmapResult (Write): Waiting\n";
          continue;
        default:
          throw std::runtime_error("RmapResult (Write) returned unknown status: " +
                                   std::to_string(static_cast<int>(reply.status())) + "\n" +
                                   reply.message());
      }
    }
  }
  throw std::runtime_error("RmapResult (Write) timed out without a response");
}

};  // namespace superhero::grpc
