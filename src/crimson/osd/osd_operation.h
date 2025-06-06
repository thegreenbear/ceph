// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include "crimson/common/operation.h"
#include "crimson/osd/pg_interval_interrupt_condition.h"
#include "crimson/osd/scheduler/scheduler.h"
#include "osd/osd_types.h"

namespace crimson::os::seastore {
  template<class OpT>
  class OperationProxyT;
}

namespace crimson::osd {

/// Ordering stages for a class of operations ordered by PG.
struct ConnectionPipeline {
  struct AwaitActive : OrderedExclusivePhaseT<AwaitActive> {
    static constexpr auto type_name =
      "ConnectionPipeline::await_active";
  } await_active;

  struct AwaitMap : OrderedExclusivePhaseT<AwaitMap> {
    static constexpr auto type_name =
      "ConnectionPipeline::await_map";
  } await_map;

  struct GetPGMapping : OrderedExclusivePhaseT<GetPGMapping> {
    static constexpr auto type_name =
      "ConnectionPipeline::get_pg_mapping";
  } get_pg_mapping;
};

struct PerShardPipeline {
  struct CreateOrWaitPG : OrderedExclusivePhaseT<CreateOrWaitPG> {
    static constexpr auto type_name =
      "PerShardPipeline::create_or_wait_pg";
  } create_or_wait_pg;
};

struct PGPeeringPipeline {
  struct AwaitMap : OrderedExclusivePhaseT<AwaitMap> {
    static constexpr auto type_name = "PeeringEvent::PGPipeline::await_map";
  } await_map;
  struct Process : OrderedExclusivePhaseT<Process> {
    static constexpr auto type_name = "PeeringEvent::PGPipeline::process";
  } process;
};

struct CommonPGPipeline {
  struct WaitPGReady : OrderedConcurrentPhaseT<WaitPGReady> {
    static constexpr auto type_name = "CommonPGPipeline:::wait_pg_ready";
  } wait_pg_ready;
  struct GetOBC : OrderedExclusivePhaseT<GetOBC> {
    static constexpr auto type_name = "CommonPGPipeline:::get_obc";
  } get_obc;
};

struct PGRepopPipeline {
  struct Process : OrderedExclusivePhaseT<Process> {
    static constexpr auto type_name = "PGRepopPipeline::process";
  } process;
  struct WaitCommit : OrderedConcurrentPhaseT<WaitCommit> {
    static constexpr auto type_name = "PGRepopPipeline::wait_repop";
  } wait_commit;
  struct SendReply : OrderedExclusivePhaseT<SendReply> {
    static constexpr auto type_name = "PGRepopPipeline::send_reply";
  } send_reply;
};

struct CommonOBCPipeline {
  struct Process : OrderedExclusivePhaseT<Process> {
    static constexpr auto type_name = "CommonOBCPipeline::process";
  } process;
  struct WaitRepop : OrderedConcurrentPhaseT<WaitRepop> {
    static constexpr auto type_name = "CommonOBCPipeline::wait_repop";
  } wait_repop;
  struct SendReply : OrderedExclusivePhaseT<SendReply> {
    static constexpr auto type_name = "CommonOBCPipeline::send_reply";
  } send_reply;
};


enum class OperationTypeCode {
  client_request = 0,
  peering_event,
  pg_advance_map,
  pg_creation,
  replicated_request,
  background_recovery,
  background_recovery_sub,
  internal_client_request,
  historic_client_request,
  historic_slow_client_request,
  logmissing_request,
  logmissing_request_reply,
  snaptrim_event,
  snaptrimobj_subevent,
  scrub_requested,
  scrub_message,
  scrub_find_range,
  scrub_reserve_range,
  scrub_scan,
  pgpct_request,
  last_op
};

static constexpr const char* const OP_NAMES[] = {
  "client_request",
  "peering_event",
  "pg_advance_map",
  "pg_creation",
  "replicated_request",
  "background_recovery",
  "background_recovery_sub",
  "internal_client_request",
  "historic_client_request",
  "historic_slow_client_request",
  "logmissing_request",
  "logmissing_request_reply",
  "snaptrim_event",
  "snaptrimobj_subevent",
  "scrub_requested",
  "scrub_message",
  "scrub_find_range",
  "scrub_reserve_range",
  "scrub_scan",
  "pgpct_request",
};

// prevent the addition of OperationTypeCode-s with no matching OP_NAMES entry:
static_assert(
  (sizeof(OP_NAMES)/sizeof(OP_NAMES[0])) ==
  static_cast<int>(OperationTypeCode::last_op));

struct InterruptibleOperation : Operation {
  template <typename ValuesT = void>
  using interruptible_future =
    ::crimson::interruptible::interruptible_future<
      ::crimson::osd::IOInterruptCondition, ValuesT>;
  using interruptor =
    ::crimson::interruptible::interruptor<
      ::crimson::osd::IOInterruptCondition>;
};

template <typename T>
struct OperationT : InterruptibleOperation {
  static constexpr const char *type_name = OP_NAMES[static_cast<int>(T::type)];
  using IRef = boost::intrusive_ptr<T>;
  using ICRef = boost::intrusive_ptr<const T>;

  unsigned get_type() const final {
    return static_cast<unsigned>(T::type);
  }

  const char *get_type_name() const final {
    return T::type_name;
  }

  virtual ~OperationT() = default;

private:
  virtual void dump_detail(ceph::Formatter *f) const = 0;
};

template <class T>
class TrackableOperationT : public OperationT<T> {
  T* that() {
    return static_cast<T*>(this);
  }
  const T* that() const {
    return static_cast<const T*>(this);
  }

protected:
  template<class EventT>
  decltype(auto) get_event() {
    // all out derivates are supposed to define the list of tracking
    // events accessible via `std::get`. This will usually boil down
    // into an instance of `std::tuple`.
    return std::get<EventT>(that()->tracking_events);
  }

  template<class EventT>
  decltype(auto) get_event() const {
    return std::get<EventT>(that()->tracking_events);
  }

  using OperationT<T>::OperationT;

  struct StartEvent : TimeEvent<StartEvent> {};
  struct CompletionEvent : TimeEvent<CompletionEvent> {};

  template <class EventT, class... Args>
  void track_event(Args&&... args) {
    // the idea is to have a visitor-like interface that allows to double
    // dispatch (backend, blocker type)
    get_event<EventT>().trigger(*that(), std::forward<Args>(args)...);
  }

  template <class BlockingEventT>
  typename BlockingEventT::template Trigger<T>
  get_trigger() {
    return {get_event<BlockingEventT>(), *that()};
  }

  template <class BlockingEventT, class InterruptorT=void, class F>
  auto with_blocking_event(F&& f) {
    auto ret = std::forward<F>(f)(get_trigger<BlockingEventT>());
    if constexpr (std::is_same_v<InterruptorT, void>) {
      return ret;
    } else {
      using ret_t = decltype(ret);
      return typename InterruptorT::template futurize_t<ret_t>{std::move(ret)};
    }
  }

public:
  static constexpr bool is_trackable = true;
  virtual bool requires_pg() const {
    return true;
  }
};

template <class T>
class PhasedOperationT : public TrackableOperationT<T> {
  using base_t = TrackableOperationT<T>;

  T* that() {
    return static_cast<T*>(this);
  }
  const T* that() const {
    return static_cast<const T*>(this);
  }

protected:
  using TrackableOperationT<T>::TrackableOperationT;

  template <class InterruptorT=void, class StageT>
  auto enter_stage(StageT& stage) {
    return this->template with_blocking_event<typename StageT::BlockingEvent,
	                                      InterruptorT>(
      [&stage, this] (auto&& trigger) {
        // delegated storing the pipeline handle to let childs to match
        // the lifetime of pipeline with e.g. ConnectedSocket (important
        // for ConnectionPipeline).
        return that()->get_handle().template enter<T>(stage, std::move(trigger));
    });
  }

  template <class StageT>
  void enter_stage_sync(StageT& stage) {
    that()->get_handle().template enter_sync<T>(
        stage, this->template get_trigger<typename StageT::BlockingEvent>());
  }

  template <class OpT>
  friend class crimson::os::seastore::OperationProxyT;

  // PGShardManager::start_pg_operation needs access to enter_stage, we can make this
  // more sophisticated later on
  friend class PGShardManager;
};

/**
 * Maintains a set of lists of all active ops.
 */
struct OSDOperationRegistry : OperationRegistryT<
  static_cast<size_t>(OperationTypeCode::last_op)
> {
  OSDOperationRegistry();

  void do_stop() override;

  void put_historic(const class ClientRequest& op);
  void _put_historic(
    op_list& list,
    const class ClientRequest& op,
    uint64_t max);

  size_t dump_historic_client_requests(ceph::Formatter* f) const;
  size_t dump_slowest_historic_client_requests(ceph::Formatter* f) const;
  void visit_ops_in_flight(std::function<void(const ClientRequest&)>&& visit);

private:
  size_t num_recent_ops = 0;
  size_t num_slow_ops = 0;
};
/**
 * Throttles set of currently running operations
 *
 * Very primitive currently, assumes all ops are equally
 * expensive and simply limits the number that can be
 * concurrently active.
 */
class OperationThrottler : public BlockerT<OperationThrottler>,
			private md_config_obs_t {
  friend BlockerT<OperationThrottler>;
  static constexpr const char* type_name = "OperationThrottler";

public:
  OperationThrottler(ConfigProxy &conf);

  std::vector<std::string> get_tracked_keys() const noexcept final;
  void handle_conf_change(const ConfigProxy& conf,
			  const std::set<std::string> &changed) final;
  void update_from_config(const ConfigProxy &conf);

  bool available() const {
    return !max_in_progress || in_progress < max_in_progress;
  }

  class ThrottleReleaser {
    OperationThrottler *parent = nullptr;
  public:
    ThrottleReleaser(OperationThrottler *parent) : parent(parent) {}
    ThrottleReleaser(const ThrottleReleaser &) = delete;
    ThrottleReleaser(ThrottleReleaser &&rhs) noexcept {
      std::swap(parent, rhs.parent);
    }

    ~ThrottleReleaser() {
      if (parent) {
	parent->release_throttle();
      }
    }
  };

  auto get_throttle(crimson::osd::scheduler::params_t params) {
    return acquire_throttle(
      params
    ).then([this] {
      return ThrottleReleaser{this};
    });
  }

private:
  void dump_detail(Formatter *f) const final;

  crimson::osd::scheduler::SchedulerRef scheduler;

  uint64_t max_in_progress = 0;
  uint64_t in_progress = 0;

  uint64_t pending = 0;

  void wake();

  seastar::future<> acquire_throttle(
    crimson::osd::scheduler::params_t params);

  void release_throttle();
};

}
