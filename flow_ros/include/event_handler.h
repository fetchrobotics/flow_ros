/**
 * @copyright 2020 Fetch Robotics Inc. All rights reserved
 * @author Brian Cairl
 *
 * @file event_handler.h
 */
#ifndef FLOW_ROS_EVENT_HANDLER_H
#define FLOW_ROS_EVENT_HANDLER_H

// C++ Standard Library
#include <chrono>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

// Flow
#include <flow/synchronizer.h>
#include <flow_ros/message_stamp_access.h>
#include <flow_ros/publisher.h>
#include <flow_ros/subscriber.h>

// Flow (implementation)
#include <flow_ros/impl/event_handler.hpp>

namespace flow_ros
{

/**
 * @brief Summary of event information from results
 */
struct EventSummary
{
  /**
   * @brief Summary state codes
   */
  enum class State
  {
    UNKNOWN,  //< unknown/unspecified state
    EXECUTED,  //< handler synchronized inputs and produced output(s)
    EXECUTION_BYPASSED,  //< handler synchronized inputs but did not run event callback
    SYNC_NEEDS_RETRY,  //< did not input sync before execution; needs retry
    SYNC_ABORTED,  //< aborted input sync before execution
    SYNC_TIMED_OUT,  //< timed out while waiting for data sync
    SYNC_ERROR_MONOTONICITY_VIOLATED,  ///< error code used to indicate that driving stamp monotonicity is violated
    SYNC_SKIPPED_ON_PRECONDITION,  ///< sync was skipped due to a queue precondition
  };

  /**
   * @brief Constructor for initialization from sync results
   *
   * @param _result  synchronizer result information
   * @param _execution_timeout  system-time point for execution timeout
   */
  EventSummary(const flow::Result<ros::Time>& _result, const std::chrono::steady_clock::time_point _execution_timeout) :
      state{[& rs = _result.state] {
        switch (rs)
        {
        case flow::State::PRIMED:
          return EventSummary::State::EXECUTED;
        case flow::State::RETRY:
          return EventSummary::State::SYNC_NEEDS_RETRY;
        case flow::State::ABORT:
          return EventSummary::State::SYNC_ABORTED;
        case flow::State::TIMEOUT:
          return EventSummary::State::SYNC_TIMED_OUT;
        case flow::State::ERROR_DRIVER_LOWER_BOUND_EXCEEDED:
          return EventSummary::State::SYNC_ERROR_MONOTONICITY_VIOLATED;
        case flow::State::SKIP_FRAME_QUEUE_PRECONDITION:
          return EventSummary::State::SYNC_SKIPPED_ON_PRECONDITION;
        default:
          return EventSummary::State::UNKNOWN;
        }
      }()},
      range{_result.range},
      execution_timeout{_execution_timeout}
  {}

  /// Event state
  State state;

  /// Capture time range
  flow::CaptureRange<ros::Time> range;

  /// Execution timeout
  std::chrono::steady_clock::time_point execution_timeout;
};


/**
 * @brief Event handler base type
 */
class EventHandlerBase
{
public:
  virtual ~EventHandlerBase() = default;

  /**
   * @brief Updates event handler
   *
   * @return summary of event states
   */
  virtual EventSummary
  update(std::chrono::steady_clock::time_point timeout = std::chrono::steady_clock::time_point::max()) = 0;

  /**
   * @brief Checks what synchronization result would be with current data
   *
   *        Performs no data waits, and skips actual capture
   *
   * @return summary of potential event states on next <code>update</code>
   */
  virtual EventSummary dry_update() = 0;

  /**
   * @brief Returns last valid update stamp
   */
  virtual ros::Time last_update_stamp() const = 0;

  /**
   * @brief Removes all events before specified time
   *
   * @param t_remove  time before which events should be removed
   */
  virtual void remove(const ros::Time& t_remove) = 0;

  /**
   * @brief Aborts all event at or before specified time
   *
   * @param t_abort  time before which events should be aborted
   */
  virtual void abort(const ros::Time& t_abort) = 0;

  /**
   * @brief Resets event handler and all managed channels
   */
  virtual void reset() = 0;

  /**
   * @brief Returns vector of publisher resources associated with EventHandler
   * @note Resource pointers in return value provide partial access to meta
   *       information about Publisher objects, but do not allow for data mutation
   */
  virtual std::vector<std::shared_ptr<const PublisherBase>> getPublishers() const = 0;

  /**
   * @brief Returns vector of subscriber resources associated with EventHandler
   * @note Resource pointers in return value provide partial access to meta
   *       information about Publisher objects, but do not allow for data mutation
   */
  virtual std::vector<std::shared_ptr<const SubscriberBase>> getSubscribers() const = 0;
};


/**
 * @brief Default dispatch container type information
 *
 *        Dispatch object collected on sync are stored into a <code>std::vector</code>
 *
 * @tparam DispatchT  message dispatch type
 */
template <typename DispatchT> struct DefaultOutputContainerTypeInfo
{
  /**
   * @brief Output container type
   */
  using Container = std::vector<DispatchT, std::allocator<DispatchT>>;

  /**
   * @brief Container output iterator type
   */
  using output_iterator_type = std::back_insert_iterator<Container>;

  /**
   * @brief Returns an appropriate container output iterator
   */
  inline static output_iterator_type get_output_iterator(Container& c) { return std::back_inserter(c); }
};


/**
 * @brief Manages a input and output channels, and runs callbacks on input synchronization
 *
 * @tparam PublisherTuple  tuple of publisher types
 * @tparam SubscriberTuple  tuple of subscriber types
 * @tparam DispatchContainerT  a container type description class; specifies what type of container to use when
 &                             capturing messages, and how to fill that container using an output iterator
 */
template <
  typename PublisherTuple,
  typename SubscriberTuple,
  template <typename> class OutputContainerTypeInfoTmpl = DefaultOutputContainerTypeInfo>
class EventHandler final : public EventHandlerBase
{
public:
  /// Tuple of publisher resource pointers
  using PublisherPtrTuple = typename detail::WrapTupleElements<std::shared_ptr, PublisherTuple>::type;

  /// Tuple of subscriber resource pointers
  using SubscriberPtrTuple = typename detail::WrapTupleElements<std::shared_ptr, SubscriberTuple>::type;

  /**
   * @brief Output data type
   *
   *        Message type returned from event callback. If:
   *        - <code>PublisherTuple</code> specifies a single output, then this is a
   *          <code>OutputMsgT::Ptr</code> associated with that publisher
   *       - <code>PublisherTuple</code> specifies multiple outputs, then this is a
   *          <code>std::tuple<Output0MsgT::Ptr, ... , OutputNMsgT::Ptr></code> associated
   *          with those publishers in the order specified by <code>PublisherTuple</code>.
   */
  using Output = typename detail::EventHandlerOutputType<PublisherTuple>::type;

  /**
   * @brief Synchronized input container type
   *
   *        <code>std::tuple<...></code> with data from each synchronized input. Inputs are ordered with
   *        respect to the ordering of the capture buffers from which they were sources, specified by
   *        <code>SubscriberTuple</code>
   */
  using Input = typename detail::EventHandlerInputType<OutputContainerTypeInfoTmpl, SubscriberTuple>::type;

  /**
   * @brief EventHandler callback options
   */
  struct Callbacks
  {
    /// Callback to run on input synchronization event
    std::function<Output(const Input&)> event_callback;

    /**
     * @brief Callback to run after synchronization for event summary introspection
     *
     *        May be used to bypass execution after synchronization, resulting in an EXECUTION_BYPASSED state
     */
    std::function<bool(EventHandlerBase&, const Input&, const EventSummary&)> pre_execute_callback;

    /**
     * @brief Event callback constructor
     *
     * @param _event_callback  callback to run after synchronization, accepting Input and producing Output data
     *
     * @note allows implicit cast from invokable type
     */
    template <typename CallbackT>
    Callbacks(CallbackT&& _event_callback) :
        event_callback{std::forward<CallbackT>(_event_callback)},
        pre_execute_callback{nullptr}
    {
      if (event_callback == nullptr)
      {
        throw std::invalid_argument{"'_event_callback' must be a valid invocable entity"};
      }
    }

    /**
     * @brief Full-callback constructor
     *
     * @param _event_callback  callback to run after synchronization, accepting Input and producing Output data
     * @param _pre_execute_callback  callback to run after synchronization, before <code>_event_callback</code>
     */
    template <typename EventCallbackT, typename PreExecuteCallbackT>
    Callbacks(EventCallbackT&& _event_callback, PreExecuteCallbackT&& _pre_execute_callback) :
        event_callback{std::forward<EventCallbackT>(_event_callback)},
        pre_execute_callback{std::forward<PreExecuteCallbackT>(_pre_execute_callback)}
    {
      if (event_callback == nullptr)
      {
        throw std::invalid_argument{"'_event_callback' must be a valid invocable entity"};
      }
    }
  };

  /**
   * @brief Required setup constructor
   *
   * @param callbacks  callback to run after input synchronization
   * @param publishers  message output channel resources
   * @param subscribers  message input channel resources
   */
  EventHandler(Callbacks callbacks, PublisherPtrTuple publishers, SubscriberPtrTuple subscribers) :
      callbacks_{std::move(callbacks)},
      publishers_{std::move(publishers)},
      subscribers_{std::move(subscribers)},
      lower_bound_stamp_{ros::TIME_MIN}
  {}

  /**
   * @brief Destructor
   */
  ~EventHandler() = default;

  /**
   * @copydoc EventHandler::dry_update
   */
  EventSummary dry_update() override
  {
    // Get synchronized messages
    const auto dry_update_result = flow::Synchronizer::dry_capture(detail::forward_as_deref_tuple(subscribers_));

    // Initialize event summary from synchronizer result
    return EventSummary{dry_update_result, std::chrono::steady_clock::time_point::max()};
  }

  /**
   * @brief Event updater method
   *
   *        On each call, this method attempts input synchronization. The behavior that follows depends on
   *        user callbacks setup on construction. If synchronization succeeds, the main event (execution)
   *        callback is invoked.
   * \n
   *        Subscribers which wait for data may wait until <code>timeout</code> unless synchronization
   *        becomre possible
   *
   * @param timeout  synchronization timeout
   *
   * @return summary of event states
   */
  EventSummary
  update(const std::chrono::steady_clock::time_point timeout = std::chrono::steady_clock::time_point::max()) override
  {
    Input sync_inputs;

    // Get synchronized messages
    const auto sync_result = flow::Synchronizer::capture(
      detail::forward_as_deref_tuple(subscribers_),
      detail::get_ouput_iterators<OutputContainerTypeInfoTmpl, SubscriberTuple>(sync_inputs),
      lower_bound_stamp_,
      timeout);

    // Update lower-bound sync time
    if (sync_result.range and sync_result.state != ::flow::State::RETRY)
    {
      lower_bound_stamp_ = std::max(lower_bound_stamp_, sync_result.range.lower_stamp);
    }

    // Initialize event summary from synchronizer result
    EventSummary event_summary{sync_result, timeout};

    // Invoke result callbacks
    if (event_summary.state == EventSummary::State::SYNC_NEEDS_RETRY)
    {
      flow::apply_every(detail::RetryReinjectHelper{}, subscribers_, sync_inputs);
      return event_summary;
    }
    else if (callbacks_.pre_execute_callback and !callbacks_.pre_execute_callback(*this, sync_inputs, event_summary))
    {
      event_summary.state = EventSummary::State::EXECUTION_BYPASSED;

      // Apply ABORT state to captor queue monitors when execution is bypassed
      flow::apply_every(
        detail::QueueMonitorUpdateHelper{sync_result.range}, detail::forward_as_deref_tuple(subscribers_));
    }
    else if (event_summary.state == EventSummary::State::EXECUTED)
    {
      flow::apply_every(
        detail::EventHandlerPublishHelper{event_summary.range.lower_stamp},
        publishers_,
        callbacks_.event_callback(sync_inputs) /*event outputs*/
      );
    }
    return event_summary;
  }

  /**
   * @brief EventHandlerBase::last_update_stamp
   */
  ros::Time last_update_stamp() const override { return lower_bound_stamp_; }

  /**
   * @copydoc EventHandlerBase::remove
   */
  void remove(const ros::Time& t_remove) override
  {
    flow::Synchronizer::remove(detail::forward_as_deref_tuple(subscribers_), t_remove);
  }

  /**
   * @copydoc EventHandlerBase::abort
   */
  void abort(const ros::Time& t_abort) override
  {
    flow::Synchronizer::abort(detail::forward_as_deref_tuple(subscribers_), t_abort);
  }

  /**
   * @copydoc EventHandlerBase::reset
   */
  void reset() override
  {
    lower_bound_stamp_ = ros::TIME_MIN;
    flow::Synchronizer::reset(detail::forward_as_deref_tuple(subscribers_));
  }

  /**
   * @copydoc EventHandlerBase::getSubscribers
   */
  std::vector<std::shared_ptr<const SubscriberBase>> getSubscribers() const override
  {
    std::vector<std::shared_ptr<const SubscriberBase>> subs;
    subs.resize(std::tuple_size<SubscriberTuple>());
    auto outitr = subs.begin();
    flow::apply_every(detail::CollectFromTuple<decltype(outitr)>{outitr}, subscribers_);
    return subs;
  }

  /**
   * @copydoc EventHandlerBase::getPublishers
   */
  std::vector<std::shared_ptr<const PublisherBase>> getPublishers() const override
  {
    std::vector<std::shared_ptr<const PublisherBase>> pubs;
    pubs.resize(std::tuple_size<PublisherTuple>());
    auto outitr = pubs.begin();
    flow::apply_every(detail::CollectFromTuple<decltype(outitr)>{outitr}, publishers_);
    return pubs;
  }

private:
  /// Event callbacks
  Callbacks callbacks_;

  /// Output channel resources
  PublisherPtrTuple publishers_;

  /// Input channel resources
  SubscriberPtrTuple subscribers_;

  /// Lower-bounding synchronization stamp
  ///
  /// This is mainly just a safety net in the event that driving inputs come in out of order
  /// after an newer message has been captured/processed
  ///
  ros::Time lower_bound_stamp_;
};

}  // namespace flow_ros

#endif  // FLOW_ROS_EVENT_HANDLER_H
