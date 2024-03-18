
#pragma once
#ifndef __CO_RETURNTYPE_H__
#define __CO_RETURNTYPE_H__

#include <concepts>       //for concept
#include <functional>     //for std::function
#include <memory>         //for ptr
#include <unordered_map>  //for unordered_map

#include "common.hpp"

namespace CO {

template <class RetType>
concept AllType = std::is_void<RetType>::value || !
std::is_void<RetType>::value;

class BaseService {
 public:
  virtual ~BaseService() {}
  // std::string info;
};

template <AllType RetType>
class Service : public BaseService {
  using FuncType = std::function<RetType()>;

 public:
  explicit Service(FuncType func) : func_(func) {}
  RetType Run() { return func_(); }

 private:
  FuncType func_;
};

class WorkerManager {
  using BaseServicePtr = std::shared_ptr<BaseService>;
  // using GUID = uint32_t;

 public:
  template <class ServiceType>
  void RegisterService(const GUID& id, const ServiceType& service) {
    // BaseServicePtr ptr = std::make_shared<ServiceType>(service);
    // auto pair = std::make_pair(id, ptr);

    // service_map_.insert(pair);
    service_map_.insert(std::pair<GUID, BaseServicePtr>(
        id, BaseServicePtr(std::make_shared<ServiceType>(service))));
  };

  template <class RetType>
  RetType Excute(const GUID& id)  //
  {
    [[likely]] if (service_map_.count(id)) {
      auto service_ptr = service_map_[id];
      Service<RetType>* true_service_ptr =
          dynamic_cast<Service<RetType>*>(service_ptr.get());
      return true_service_ptr->Run();
    } else {
      return RetType();
    }
  }

 public:
  ~WorkerManager() {}

 private:
  std::unordered_map<GUID, BaseServicePtr> service_map_;
};
}  // namespace CO
#endif