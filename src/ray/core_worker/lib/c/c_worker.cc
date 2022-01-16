#include "c_worker.h"

#include "util.h"
#include "config.h"

#include <stdint.h>
#include <iostream>
#include <boost/algorithm/string.hpp>

#include "ray/common/id.h"
#include "ray/core_worker/core_worker.h"
#include "ray/gcs/gcs_client/global_state_accessor.h"

using namespace std;

// Use anonymous namespace to enforce Rule of One Definition
// for static global variables
namespace {
// TODO: We can also register c_worker_on_shutdown and c_worker_gc_collect
//
// We need additional wrappers with the right function signature to marshall
// and unmarshall data to/from C-ABI callback (or maybe this is much simpler if
// they are simply `std::function<void()>`)
static c_worker_ExecuteCallback c_worker_execute;
static ray::core::CoreWorkerOptions stored_worker_options;
static Config stored_config;
}

int c_worker_RegisterExecutionCallback(c_worker_ExecuteCallback callback) {
    c_worker_execute = callback;
    return 1;
}

// We assume that the byte array is properly allocated, i.e.
// has len == ID::Size()
template <typename ID>
inline ID ByteArrayToId(const char *bytes) {
  std::string str(bytes, ID::Size());
  return ID::FromBinary(str);
}

DataBuffer *AllocateDataBuffer(uint8_t *ptr, int size) {
  auto db = new DataBuffer();
  db->p = ptr;
  db->size = size;
  return db;
}

// TODO(jon-chuang): what is the purpose of this RAY_EXPORT?

// How do we ensure that this is destructed...?
RAY_EXPORT DataValue* c_worker_AllocateDataValue(uint8_t *data_ptr, size_t data_size,
                                                 uint8_t *meta_ptr, size_t meta_size) {
  auto dv = new DataValue();
  dv->data = AllocateDataBuffer(data_ptr, data_size);
  dv->meta = AllocateDataBuffer(meta_ptr, meta_size);
  return dv;
}

RAY_EXPORT void c_worker_Log(const char *msg) {
  RAY_LOG(INFO) << msg;
}

RAY_EXPORT void c_worker_InitConfig(int workerMode, int language, int num_workers,
                                    char *code_search_path, char *head_args,
                                    int argc, char** argv) {
  stored_worker_options.worker_type = static_cast<ray::core::WorkerType>(workerMode);
  stored_worker_options.language = static_cast<ray::Language>(language);
  stored_worker_options.num_workers = num_workers;

  InitOptions(&stored_config, &stored_worker_options, code_search_path, head_args, argc, argv);
}


ray::Status ExecutionCallback(
  ray::rpc::TaskType task_type, const std::string task_name,
  const ray::core::RayFunction &ray_function,
  const std::unordered_map<std::string, double> &required_resources,
  const std::vector<std::shared_ptr<ray::RayObject>> &args,
  const std::vector<ray::rpc::ObjectReference> &arg_refs,
  const std::vector<ObjectID> &return_ids, const std::string &debugger_breakpoint,
  std::vector<std::shared_ptr<ray::RayObject>> *results,
  std::shared_ptr<ray::LocalMemoryBuffer> &creation_task_exception_pb,
  bool *is_application_level_error,
  const std::vector<ray::ConcurrencyGroup> &defined_concurrency_groups,
  const std::string name_of_concurrency_group_to_execute
) {
  // convert RayFunction
  auto function_descriptor = ray_function.GetFunctionDescriptor();

  auto typed_descriptor = function_descriptor->As<ray::RustFunctionDescriptor>();

  char *fd_data[1];
  fd_data[0] = const_cast<char *>(typed_descriptor->FunctionName().c_str());
  RaySlice fd_list;
  fd_list.data = fd_data;
  fd_list.len = 1;
  fd_list.cap = 1;

  std::vector<DataValue *> args_array_list;
  for (auto &it : args) {
    auto data = it->GetData();
    // auto meta = it->GetMetadata();
    args_array_list.push_back(c_worker_AllocateDataValue(
        data->Data(), data->Size(), nullptr, 0));//meta->Data(), meta->Size()));
  }

  RaySlice execute_args;
  execute_args.data = &args_array_list[0];
  execute_args.len = args_array_list.size();
  execute_args.cap = args_array_list.size();


  std::vector<std::shared_ptr<DataValue>> return_value_list;
  for (size_t i = 0; i < return_ids.size(); i++) {
    return_value_list.push_back(make_shared<DataValue>());
  }
  // return_value_list[0].reset(buf);
  RaySlice execute_return_value_list;
  execute_return_value_list.data = &return_value_list[0];
  execute_return_value_list.cap = return_ids.size();
  execute_return_value_list.len = return_ids.size();

  if (c_worker_execute == nullptr) {
    // TODO (jon-chuang): RAY_THROW.
    assert (false);
  }
  c_worker_execute(task_type, fd_list, execute_args, execute_return_value_list);
  results->clear();
  for (size_t i = 0; i < return_value_list.size(); i++) {
    auto &result_id = return_ids[i];
    auto &return_value = return_value_list[i];
    // How do you prevent memory bugs without needing to copy data?
    std::shared_ptr<ray::Buffer> data_buffer =
        std::make_shared<ray::LocalMemoryBuffer>(
            reinterpret_cast<uint8_t *>(return_value->data->p),
            return_value->data->size, true);
    std::shared_ptr<ray::Buffer> meta_buffer = nullptr;
        // std::make_shared<ray::LocalMemoryBuffer>(
        //     reinterpret_cast<uint8_t *>(return_value->meta->p),
        //     return_value->meta->size, false);

    std::vector<ray::ObjectID> contained_object_ids;
    auto contained_object_refs =
        ray::core::CoreWorkerProcess::GetCoreWorker().GetObjectRefs(
            contained_object_ids);

    auto value = std::make_shared<ray::RayObject>(data_buffer, meta_buffer,
                                                  contained_object_refs);
    results->emplace_back(value);
    int64_t task_output_inlined_bytes = 0;
    RAY_CHECK_OK(ray::core::CoreWorkerProcess::GetCoreWorker().AllocateReturnObject(
        result_id, value->GetData()->Size(), value->GetMetadata(),
        contained_object_ids, &task_output_inlined_bytes, &value));
    RAY_CHECK_OK(ray::core::CoreWorkerProcess::GetCoreWorker().SealReturnObject(
        result_id, value));
  }
  return ray::Status::OK();
};

RAY_EXPORT void c_worker_Initialize() {
  ray::core::CoreWorkerOptions options = stored_worker_options;
  auto redis_ip = stored_config.redis_ip;
  if (options.worker_type == ray::core::WorkerType::DRIVER
    && stored_config.redis_ip.empty()
  ) {
    redis_ip = "127.0.0.1";
    StartRayNode(stored_config.redis_port,
                 stored_config.redis_password,
                 stored_config.head_args);
  }
  if (redis_ip == "127.0.0.1") {
    redis_ip = GetNodeIpAddress();
  }

  std::string redis_address = redis_ip + ":" + std::to_string(stored_config.redis_port);
  std::string node_ip = options.node_ip_address;
  if (node_ip.empty()) {
    if (!stored_config.redis_ip.empty()) {
      node_ip = GetNodeIpAddress(redis_address);
    } else {
      node_ip = GetNodeIpAddress();
    }
  }

  auto global_state_accessor =
      CreateGlobalStateAccessor(redis_address, stored_config.redis_password);
  if (options.worker_type == ray::core::WorkerType::DRIVER) {
    std::string node_to_connect;
    auto status =
        global_state_accessor->GetNodeToConnectForDriver(node_ip, &node_to_connect);
    RAY_CHECK_OK(status);
    ray::rpc::GcsNodeInfo node_info;
    node_info.ParseFromString(node_to_connect);
    options.raylet_socket = node_info.raylet_socket_name();
    options.store_socket = node_info.object_store_socket_name();
    options.node_manager_port = node_info.node_manager_port();
    if (options.job_id == ray::JobID::Nil()) {
      options.job_id = global_state_accessor->GetNextJobID();
    }
  }

  RAY_CHECK(!options.raylet_socket.empty());
  RAY_CHECK(!options.store_socket.empty());
  RAY_CHECK(options.node_manager_port > 0);

  std::string log_dir = options.log_dir;
  if (log_dir.empty()) {
    std::string session_dir = stored_config.session_dir;
    if (session_dir.empty()) {
      session_dir =
          *global_state_accessor->GetInternalKV("session", "session_dir");
      RAY_CHECK(!session_dir.empty());
    }
    log_dir = session_dir + "/logs";
    options.log_dir = log_dir;
    RAY_LOG(INFO) << "Session dir: " << session_dir;
  }
  RAY_LOG(INFO) << "Log dir: " << options.log_dir;
  options.enable_logging = true;

  ray::rpc::JobConfig job_config;
  for (const auto &path : stored_config.code_search_path) {
    job_config.add_code_search_path(path);
  }
  std::string serialized_job_config;
  RAY_CHECK(job_config.SerializeToString(&serialized_job_config));

  options.serialized_job_config = serialized_job_config;
  options.gcs_options =
      ray::gcs::GcsClientOptions(redis_ip, stored_config.redis_port, stored_config.redis_password);
  options.install_failure_signal_handler = true;
  options.node_ip_address = node_ip;
  options.raylet_ip_address = node_ip;
  options.driver_name = stored_config.driver_name;
  options.task_execution_callback = ExecutionCallback;
  //  options.on_worker_shutdown = on_worker_shutdown;
  //  options.gc_collect = gc_collect;
  options.num_workers = 1;
  options.serialized_job_config = serialized_job_config;
  options.metrics_agent_port = -1;

  ray::core::CoreWorkerProcess::Initialize(options);
}

RAY_EXPORT void c_worker_Run() {
  ray::core::CoreWorkerProcess::RunTaskExecutionLoop();
  // _Exit(0);
}

//
// RAY_EXPORT void *c_worker_CreateGlobalStateAccessor(char *redis_address,
//                                                      char *redis_password) {
//   ray::gcs::GlobalStateAccessor *gcs_accessor =
//       new ray::gcs::GlobalStateAccessor(redis_address, redis_password);
//   return gcs_accessor;
// }
//
// RAY_EXPORT bool c_worker_GlobalStateAccessorConnet(void *p) {
//   auto *gcs_accessor = static_cast<ray::gcs::GlobalStateAccessor *>(p);
//   return gcs_accessor->Connect();
// }
//
// RAY_EXPORT int c_worker_GetNextJobID(void *p) {
//   auto *gcs_accessor = static_cast<ray::gcs::GlobalStateAccessor *>(p);
//   auto job_id = gcs_accessor->GetNextJobID();
//   return job_id.ToInt();
// }
//
// RAY_EXPORT char *c_worker_GlobalStateAccessorGetInternalKV(void *p, char *key) {
//   auto *gcs_accessor = static_cast<ray::gcs::GlobalStateAccessor *>(p);
//   auto value = gcs_accessor->GetInternalKV(key);
//   if (value != nullptr) {
//     std::string *v = value.release();
//     int len = strlen(v->c_str());
//     char *result = (char *)malloc(len + 1);
//     std::memcpy(result, v->c_str(), len);
//     return result;
//   }
//   return nullptr;
// }
//
// RAY_EXPORT int c_worker_GetNodeToConnectForDriver(void *p, char *node_ip_address,
//                                                    char **result) {
//   RAY_LOG(DEBUG) << "Get nodeinfo:" << node_ip_address;
//   auto *gcs_accessor = static_cast<ray::gcs::GlobalStateAccessor *>(p);
//   std::string node_to_connect;
//   auto status =
//       gcs_accessor->GetNodeToConnectForDriver(node_ip_address, &node_to_connect);
//   if (!status.ok()) {
//     RAY_LOG(FATAL) << "Failed to get node to connect for driver:" << status.message();
//     return 0;
//   }
//   RAY_LOG(DEBUG) << "Got nodeinfo:" << node_to_connect;
//   int result_length = node_to_connect.length();
//   *result = (char *)malloc(result_length + 1);
//   memcpy(*result, node_to_connect.c_str(), result_length);
//   return result_length;
// }
//
// RAY_EXPORT int c_worker_CreateActor(char *type_name, char **result) {
//   std::vector<std::string> function_descriptor_list = {type_name};
//   ray::FunctionDescriptor function_descriptor =
//       ray::FunctionDescriptorBuilder::FromVector(ray::rpc::RUST,
//                                                  function_descriptor_list);
//   ActorID actor_id;
//   ray::core::RayFunction ray_function =
//       ray::core::RayFunction(ray::rpc::RUST, function_descriptor);
//   // TODO
//   std::string full_name = "";
//   std::string ray_namespace = "";
//   ray::core::ActorCreationOptions actor_creation_options{0,
//                                                          0,
//                                                          static_cast<int>(1),
//                                                          {},
//                                                          {},
//                                                          {},
//                                                          /*is_detached=*/false,
//                                                          full_name,
//                                                          ray_namespace,
//                                                          /*is_asyncio=*/false};
//   // RUST struct constructor's args is always empty.
//   auto status = ray::core::CoreWorkerProcess::GetCoreWorker().CreateActor(
//       ray_function, {}, actor_creation_options,
//       /*extension_data*/ "", &actor_id);
//   if (!status.ok()) {
//     RAY_LOG(FATAL) << "Failed to create actor:" << status.message()
//                    << " for:" << type_name;
//     return 0;
//   }
//   int result_length = actor_id.Size();
//   *result = (char *)malloc(result_length + 1);
//   memcpy(*result, (char *)actor_id.Data(), result_length);
//   return result_length;
// }
//
inline const std::shared_ptr<ray::Buffer> DataBufferToRayBuffer(DataBuffer *db) {
  return std::make_shared<ray::LocalMemoryBuffer>(reinterpret_cast<uint8_t *>(db->p),
                                                  db->size, false);
}

inline std::shared_ptr<ray::RayObject> DataValueToRayObject(DataValue *in) {
  std::vector<ObjectID> contained_object_ids;
  auto contained_object_refs =
      ray::core::CoreWorkerProcess::GetCoreWorker().GetObjectRefs(contained_object_ids);
  std::shared_ptr<ray::Buffer> data = DataBufferToRayBuffer(in->data);
  std::shared_ptr<ray::Buffer> meta = DataBufferToRayBuffer(in->meta);
  return std::make_shared<ray::RayObject>(data, meta, contained_object_refs);
}
//
// RAY_EXPORT int c_worker_SubmitActorTask(void *actor_id, char *method_name,
//                                          DataValue **input_values, int num_input_value,
//                                          int num_returns, void **object_ids) {
//   auto actor_id_obj = ByteArrayToId<ray::ActorID>((char *)actor_id);
//   std::vector<std::string> function_descriptor_list = {method_name};
//   ray::FunctionDescriptor function_descriptor =
//       ray::FunctionDescriptorBuilder::FromVector(ray::rpc::RUST,
//                                                  function_descriptor_list);
//   ray::core::RayFunction ray_function =
//       ray::core::RayFunction(ray::rpc::RUST, function_descriptor);
//   std::string name = "";
//   std::unordered_map<std::string, double> resources;
//   ray::core::TaskOptions task_options{name, num_returns, resources};
//   std::vector<std::unique_ptr<ray::TaskArg>> args;
//
//   for (size_t i = 0; i < num_input_value; i++) {
//     //    auto java_id = env->GetObjectField(arg, java_function_arg_id);
//     //    if (java_id) {
//     //      auto java_id_bytes = static_cast<jbyteArray>(
//     //          env->CallObjectMethod(java_id, java_base_id_get_bytes));
//     //      RAY_CHECK_JAVA_EXCEPTION(env);
//     //      auto id = JavaByteArrayToId<ObjectID>(env, java_id_bytes);
//     //      auto java_owner_address =
//     //          env->GetObjectField(arg, java_function_arg_owner_address);
//     //      RAY_CHECK(java_owner_address);
//     //      auto owner_address = JavaProtobufObjectToNativeProtobufObject<rpc::Address>(
//     //          env, java_owner_address);
//     //      return std::unique_ptr<TaskArg>(new TaskArgByReference(id, owner_address));
//     //    }
//     //    auto java_value =
//     //        static_cast<jbyteArray>(env->GetObjectField(arg, java_function_arg_value));
//     //    RAY_CHECK(java_value) << "Both id and value of FunctionArg are null.";
//     auto value = DataValueToRayObject(static_cast<DataValue *>(input_values[i]));
//     args.push_back(std::unique_ptr<ray::TaskArg>(new ray::TaskArgByValue(value)));
//   }
//   std::vector<ObjectID> obj_ids;
//   ray::core::CoreWorkerProcess::GetCoreWorker().SubmitActorTask(
//       actor_id_obj, ray_function, args, task_options, &obj_ids);
//
//   int object_id_size = ObjectID::Size();
//   for (size_t i = 0; i < obj_ids.size(); i++) {
//     void *result = malloc(object_id_size);
//     memcpy(result, (char *)obj_ids[i].Data(), object_id_size);
//     RAY_LOG(DEBUG) << "return object id:" << result;
//     object_ids[i] = result;
//   }
//   return 0;
// }
//

// TODO: make this more generic for char** method_names
RAY_EXPORT int c_worker_SubmitTask(char *method_name, bool *input_is_ref,
                                   DataValue **input_values, char **input_refs,
                                   int num_input_value,
                                   int num_returns, char **object_ids) {
  std::vector<std::string> function_descriptor_list = { method_name };
  ray::FunctionDescriptor function_descriptor =
      ray::FunctionDescriptorBuilder::FromVector(ray::rpc::RUST,
                                                 function_descriptor_list);
  ray::core::RayFunction ray_function =
      ray::core::RayFunction(ray::rpc::RUST, function_descriptor);
  std::string name = "";
  std::unordered_map<std::string, double> resources;
  ray::core::TaskOptions task_options{name, num_returns, resources};
  std::vector<std::unique_ptr<ray::TaskArg>> args;

  for (int i = 0; i < num_input_value; i++) {
    if (input_is_ref[i]) {
      auto obj_id = ByteArrayToId<ray::ObjectID>(object_ids[i]);
      auto owner_id = ray::core::CoreWorkerProcess::GetCoreWorker().GetOwnerAddress(obj_id);
      args.push_back(std::unique_ptr<ray::TaskArg>(
                     new ray::TaskArgByReference(obj_id, owner_id, /*call_site=*/"")));
    } else {
      auto value = DataValueToRayObject(static_cast<DataValue *>(input_values[i]));
      args.push_back(std::unique_ptr<ray::TaskArg>(new ray::TaskArgByValue(value)));
    }
  }
  ray::BundleID bundle_id = std::make_pair(ray::PlacementGroupID::Nil(), -1);
  ray::rpc::SchedulingStrategy scheduling_strategy;
  scheduling_strategy.mutable_default_scheduling_strategy();
  if (!bundle_id.first.IsNil()) {
    auto placement_group_scheduling_strategy =
        scheduling_strategy.mutable_placement_group_scheduling_strategy();
    placement_group_scheduling_strategy->set_placement_group_id(bundle_id.first.Binary());
    placement_group_scheduling_strategy->set_placement_group_bundle_index(
        bundle_id.second);
    placement_group_scheduling_strategy->set_placement_group_capture_child_tasks(false);
  }

  auto return_refs = ray::core::CoreWorkerProcess::GetCoreWorker().SubmitTask(
      ray_function, args, task_options, /*max_retries=*/1, false, scheduling_strategy, "");

  int object_id_size = ObjectID::Size();
  for (size_t i = 0; i < return_refs.size(); i++) {
    char *result = (char *)malloc(object_id_size);
    memcpy(result, (char *)return_refs[i].object_id().data(), object_id_size);
    RAY_LOG(DEBUG) << "return object id:" << result;
    object_ids[i] = result;
  }
  return 0;
}

DataBuffer *RayBufferToDataBuffer(const std::shared_ptr<ray::Buffer> buffer) {
  auto data_db = new DataBuffer();
  data_db->p = buffer->Data();
  data_db->size = buffer->Size();
  return data_db;
}

RAY_EXPORT void c_worker_AddLocalRef(const char* id) {
  // TODO: again, shouldn't this be uint8_t?
  auto obj_id = ByteArrayToId<ray::ObjectID>(id);
  ray::core::CoreWorkerProcess::GetCoreWorker().AddLocalReference(obj_id);
}

RAY_EXPORT void c_worker_RemoveLocalRef(const char* id) {
  // TODO: again, shouldn't this be uint8_t?
  auto obj_id = ByteArrayToId<ray::ObjectID>(id);
  ray::core::CoreWorkerProcess::GetCoreWorker().RemoveLocalReference(obj_id);
}

RAY_EXPORT int c_worker_Get(const char* const object_ids[], int object_ids_size, int timeout,
                            DataValue **objects) {
  std::vector<ray::ObjectID> object_ids_data;
  for (int i = 0; i < object_ids_size; i++) {
    auto object_id_obj = ByteArrayToId<ray::ObjectID>(object_ids[i]);
    RAY_LOG(DEBUG) << "try to get object:" << object_id_obj;
    object_ids_data.emplace_back(object_id_obj);
  }
  std::vector<std::shared_ptr<ray::RayObject>> results;
  auto status = ray::core::CoreWorkerProcess::GetCoreWorker().Get(object_ids_data,
                                                                  timeout, &results);
  // todo throw error, not exit now
  if (!status.ok()) {
    RAY_LOG(FATAL) << "Failed to get object";
    return 1;
  }
  for (size_t i = 0; i < results.size(); i++) {
    auto rv = new DataValue();
    rv->data = RayBufferToDataBuffer(results[i]->GetData());
    // rv->meta = RayBufferToDataBuffer(results[i]->GetMetadata());
    objects[i] = rv;
  }
  return 0;
}

RAY_EXPORT int c_worker_Put(char **object_ids, int timeout, DataValue **objects, int objects_size) {
  ObjectID object_id;
  DataValue* data = objects[0];
  auto buffer = std::make_shared<::ray::LocalMemoryBuffer>(
      reinterpret_cast<uint8_t *>(data->data->p), data->data->size, true);

  auto status = ray::core::CoreWorkerProcess::GetCoreWorker().Put(
      ::ray::RayObject(buffer, nullptr, std::vector<ray::rpc::ObjectReference>()), {},
      &object_id);

  // TODO (jon-chuang): RAY_THROW instead
  if (!status.ok()) {
    RAY_LOG(INFO) << "Put object error: " << status.ToString();
    return 1;
  } else {
    RAY_LOG(INFO) << "Put object success: " << status.ToString();
  }

  int object_id_size = ObjectID::Size();
  char *result = (char *) malloc(sizeof(char) * object_id_size);
  memcpy(result, (char *)object_id.Data(), object_id_size);
  object_ids[0] = result;

  ray::core::CoreWorkerProcess::GetCoreWorker().AddLocalReference(object_id);

  return 0;
}

RAY_EXPORT void c_worker_Shutdown() {
  ray::core::CoreWorkerProcess::Shutdown();
  if (stored_worker_options.worker_type == ray::core::WorkerType::DRIVER) {
    StopRayNode();
  }
}
