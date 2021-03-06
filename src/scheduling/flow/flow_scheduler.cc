/*
 * Firmament
 * Copyright (c) The Firmament Authors.
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 * LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR
 * A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache Version 2.0 License for specific language governing
 * permissions and limitations under the License.
 */

// Implementation of a Quincy-style min-cost flow scheduler.

#include "scheduling/flow/flow_scheduler.h"

#include <boost/timer/timer.hpp>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "base/common.h"
#include "base/types.h"
#include "base/units.h"
#include "misc/map-util.h"
#include "misc/pb_utils.h"
#include "misc/utils.h"
#include "misc/string_utils.h"
#include "storage/object_store_interface.h"
#include "scheduling/knowledge_base.h"
#include "scheduling/scheduling_event_notifier_interface.h"
#include "scheduling/flow/cost_models.h"
#include "scheduling/flow/cost_model_interface.h"

#define SIMULATION_START_TIME 600000000

DEFINE_int32(flow_scheduling_cost_model, 0,
             "Flow scheduler cost model to use. "
             "Values: 0 = TRIVIAL, 1 = RANDOM, 2 = SJF, 3 = QUINCY, "
             "4 = WHARE, 5 = COCO, 6 = OCTOPUS, 7 = VOID, 8 = NET, "
             "9 = QUINCY_INTERFERENCE");
DEFINE_uint64(max_solver_runtime, 100000000,
              "Maximum runtime of the solver in u-sec");
DEFINE_int64(time_dependent_cost_update_frequency, 10000000ULL,
             "Update frequency for time-dependent costs, in microseconds.");
DEFINE_bool(gather_unscheduled_tasks, true, "Gather unscheduled tasks");
DEFINE_bool(debug_cost_model, false,
            "Store cost model debug info in CSV files.");
DEFINE_uint64(purge_unconnected_ec_frequency, 10, "Frequency in solver runs "
              "at which to purge unconnected EC nodes");
DEFINE_bool(update_resource_topology_capacities, false,
            "True if the arc capacities of the resource topology should be "
            "updated after every scheduling round");
DEFINE_uint64(max_tasks_per_pu, 1,
              "The maximum number of tasks we can schedule per PU");
DEFINE_string(solver_runtime_accounting_mode, "algorithm",
              "Options: algorithm | solver | firmament. Modes to account for "
              "scheduling duration in simulations");
DEFINE_bool(reschedule_tasks_upon_node_failure, true, "True if tasks that were "
            "running on failed nodes should be rescheduled");

DECLARE_string(flow_scheduling_solver);
DECLARE_bool(flowlessly_flip_algorithms);
DEFINE_bool(resource_stats_update_based_on_resource_reservation, true,
            "Set this false when you have external machine stats server");
DEFINE_bool(pod_affinity_antiaffinity_symmetry, false, "Enable pod affinity/anti-affinity symmetry");

namespace firmament {
namespace scheduler {

using common::pb_to_set;
using store::ObjectStoreInterface;

FlowScheduler::FlowScheduler(
    shared_ptr<JobMap_t> job_map,
    shared_ptr<ResourceMap_t> resource_map,
    ResourceTopologyNodeDescriptor* resource_topology,
    shared_ptr<ObjectStoreInterface> object_store,
    shared_ptr<TaskMap_t> task_map,
    shared_ptr<KnowledgeBase> knowledge_base,
    shared_ptr<TopologyManager> topo_mgr,
    MessagingAdapterInterface<BaseMessage>* m_adapter,
    SchedulingEventNotifierInterface* event_notifier,
    ResourceID_t coordinator_res_id,
    const string& coordinator_uri,
    TimeInterface* time_manager,
    TraceGenerator* trace_generator,
    unordered_map<string, unordered_map<string, vector<TaskID_t>>>* labels_map,
    vector<TaskID_t> *affinity_antiaffinity_tasks)
    : EventDrivenScheduler(job_map, resource_map, resource_topology,
                           object_store, task_map, knowledge_base, topo_mgr,
                           m_adapter, event_notifier, coordinator_res_id,
                           coordinator_uri, time_manager, trace_generator,
                           labels_map, affinity_antiaffinity_tasks),
      topology_manager_(topo_mgr),
      last_updated_time_dependent_costs_(0ULL),
      leaf_res_ids_(new unordered_set<ResourceID_t,
                      boost::hash<boost::uuids::uuid>>),
      dimacs_stats_(new DIMACSChangeStats),
      solver_run_cnt_(0) {
  // Select the cost model to use
  VLOG(1) << "Set cost model to use in flow graph to \""
          << FLAGS_flow_scheduling_cost_model << "\"";

  switch (FLAGS_flow_scheduling_cost_model) {
    case CostModelType::COST_MODEL_TRIVIAL:
      cost_model_ = new TrivialCostModel(resource_map, task_map, leaf_res_ids_);
      VLOG(1) << "Using the trivial cost model";
      break;
    case CostModelType::COST_MODEL_RANDOM:
      cost_model_ = new RandomCostModel(resource_map, task_map, leaf_res_ids_);
      VLOG(1) << "Using the random cost model";
      break;
    case CostModelType::COST_MODEL_COCO:
      cost_model_ = new CocoCostModel(resource_map, *resource_topology,
                                      task_map, leaf_res_ids_, knowledge_base_,
                                      time_manager_);
      VLOG(1) << "Using the coco cost model";
      break;
    case CostModelType::COST_MODEL_SJF:
      cost_model_ = new SJFCostModel(resource_map, task_map, leaf_res_ids_,
                                     knowledge_base_, time_manager_);
      VLOG(1) << "Using the SJF cost model";
      break;
    case CostModelType::COST_MODEL_QUINCY:
      cost_model_ = new QuincyCostModel(resource_map, job_map, task_map,
                                        knowledge_base_, trace_generator_,
                                        time_manager_);
      VLOG(1) << "Using the Quincy cost model";
      break;
    case CostModelType::COST_MODEL_WHARE:
      cost_model_ = new WhareMapCostModel(resource_map, task_map,
                                          knowledge_base_, time_manager_);
      VLOG(1) << "Using the Whare-Map cost model";
      break;
    case CostModelType::COST_MODEL_OCTOPUS:
      cost_model_ = new OctopusCostModel(resource_map, task_map);
      VLOG(1) << "Using the octopus cost model";
      break;
    case CostModelType::COST_MODEL_VOID:
      cost_model_ = new VoidCostModel(resource_map, task_map);
      VLOG(1) << "Using the void cost model";
      break;
    case CostModelType::COST_MODEL_NET:
      cost_model_ = new NetCostModel(resource_map, task_map, knowledge_base);
      VLOG(1) << "Using the net cost model";
      break;
    case CostModelType::COST_MODEL_CPU:
      cost_model_ = 
          new CpuCostModel(resource_map, task_map, knowledge_base, labels_map);
      VLOG(1) << "Using the cpu cost model";
      break;
    case CostModelType::COST_MODEL_QUINCY_INTERFERENCE:
      cost_model_ =
        new QuincyInterferenceCostModel(resource_map, job_map, task_map,
                                        knowledge_base_, trace_generator_,
                                        time_manager_);
      VLOG(1) << "Using the Quincy interference cost model";
      break;
    default:
      LOG(FATAL) << "Unknown flow scheduling cost model specificed "
                 << "(" << FLAGS_flow_scheduling_cost_model << ")";
  }

  flow_graph_manager_.reset(
      new FlowGraphManager(cost_model_, leaf_res_ids_, time_manager_,
                           trace_generator_, dimacs_stats_));
  cost_model_->SetFlowGraphManager(flow_graph_manager_);

  // Set up the initial flow graph
  flow_graph_manager_->AddResourceTopology(resource_topology);
  // Set up the dispatcher, which starts the flow solver
  solver_dispatcher_ = new SolverDispatcher(flow_graph_manager_, false);
}

FlowScheduler::~FlowScheduler() {
  delete dimacs_stats_;
  delete cost_model_;
  delete solver_dispatcher_;
  delete leaf_res_ids_;
}

uint64_t FlowScheduler::ApplySchedulingDeltas(
    const vector<SchedulingDelta*>& deltas) {
  uint64_t num_scheduled = 0;
  // Perform the necessary actions to apply the scheduling changes.
  VLOG(2) << "Applying " << deltas.size() << " scheduling deltas...";
  for (auto& delta : deltas) {
    VLOG(2) << "Processing delta of type " << delta->type();
    ResourceID_t res_id = ResourceIDFromString(delta->resource_id());
    TaskDescriptor* td_ptr = FindPtrOrNull(*task_map_, delta->task_id());
    ResourceStatus* rs = FindPtrOrNull(*resource_map_, res_id);
    CHECK_NOTNULL(td_ptr);
    CHECK_NOTNULL(rs);
    JobDescriptor* jd = FindOrNull(*job_map_,
                                   JobIDFromString(td_ptr->job_id()));
    CHECK_NOTNULL(jd);
    if (jd->is_gang_scheduling_job()) {
      if (td_ptr->has_affinity()
          && (td_ptr->affinity().has_pod_affinity()
          || td_ptr->affinity().has_pod_anti_affinity())) {
        if (queue_based_schedule) {
          vector<SchedulingDelta>* delta_vec =
                            FindOrNull(affinity_job_to_deltas_, jd);
          if (delta_vec) {
            if (affinity_delta_tasks.find(delta->task_id()) 
                                     == affinity_delta_tasks.end()) {
              delta_vec->push_back(*delta);
              affinity_delta_tasks.insert(delta->task_id());
            }
          }
        }
      } else {
        uint64_t scheduled_tasks_count = jd->scheduled_tasks_count();
        if (scheduled_tasks_count < jd->min_number_of_tasks()) {
          jd->set_scheduled_tasks_count(--scheduled_tasks_count);
          delta->set_type(SchedulingDelta::NOOP);
          if (delta_jobs.find(jd) == delta_jobs.end()) {
            delta_jobs.insert(jd);
          }
          continue;
        }
      }
    }
    if (delta->type() == SchedulingDelta::NOOP) {
      // We should not get any NOOP deltas as they get filtered before.
      continue;
    } else if (delta->type() == SchedulingDelta::PLACE) {
      // Update the knowlege base with resource stats samples based on tasks
      // resource requeset, when we do not have external dynamic resource
      // stats provider like heapster in kubernetes.
      if (FLAGS_resource_stats_update_based_on_resource_reservation) {
        ResourceStats resource_stats;
        CpuStats* cpu_stats = resource_stats.add_cpus_stats();
        bool have_sample = knowledge_base_->GetLatestStatsForMachine(
            ResourceIDFromString(rs->mutable_topology_node()->parent_id()),
            &resource_stats);
        if (have_sample) {
          cpu_stats->set_cpu_allocatable(
              cpu_stats->cpu_allocatable() -
              td_ptr->resource_request().cpu_cores());
          resource_stats.set_mem_allocatable(
              resource_stats.mem_allocatable() -
              td_ptr->resource_request().ram_cap());
          // ephemeral storage
          resource_stats.set_ephemeral_storage_allocatable(
              resource_stats.ephemeral_storage_allocatable() -
              td_ptr->resource_request().ephemeral_storage());
          double cpu_utilization =
              (cpu_stats->cpu_capacity() - cpu_stats->cpu_allocatable()) /
              (double)cpu_stats->cpu_capacity();
          cpu_stats->set_cpu_utilization(cpu_utilization);
          double mem_utilization = (resource_stats.mem_capacity() -
                                    resource_stats.mem_allocatable()) /
                                   (double)resource_stats.mem_capacity();
          resource_stats.set_mem_utilization(mem_utilization);
          double ephemeral_storage_utilization = (resource_stats.ephemeral_storage_capacity() -
                                    resource_stats.ephemeral_storage_allocatable()) /
                                   (double)resource_stats.ephemeral_storage_capacity();
          resource_stats.set_ephemeral_storage_utilization(ephemeral_storage_utilization);
          knowledge_base_->AddMachineSample(resource_stats);
        }
      }
      // Tag the job to which this task belongs as running
      JobDescriptor* jd =
          FindOrNull(*job_map_, JobIDFromString(td_ptr->job_id()));
      if (jd->state() != JobDescriptor::RUNNING)
        jd->set_state(JobDescriptor::RUNNING);
      HandleTaskPlacement(td_ptr, rs->mutable_descriptor());
      num_scheduled++;
    } else if (delta->type() == SchedulingDelta::PREEMPT) {
      // Update the knowlege base with resource stats samples based on tasks
      // resource requeset, when we do not have external dynamic resource
      // stats provider like heapster in kubernetes.
      if (FLAGS_resource_stats_update_based_on_resource_reservation) {
        ResourceStats resource_stats;
        CpuStats* cpu_stats = resource_stats.add_cpus_stats();
        bool have_sample = knowledge_base_->GetLatestStatsForMachine(
            ResourceIDFromString(rs->mutable_topology_node()->parent_id()),
            &resource_stats);
        if (have_sample) {
          cpu_stats->set_cpu_allocatable(
              cpu_stats->cpu_allocatable() +
              td_ptr->resource_request().cpu_cores());
          resource_stats.set_mem_allocatable(
              resource_stats.mem_allocatable() +
              td_ptr->resource_request().ram_cap());
          resource_stats.set_ephemeral_storage_allocatable(
              resource_stats.ephemeral_storage_allocatable() +
              td_ptr->resource_request().ephemeral_storage());
          double cpu_utilization =
              (cpu_stats->cpu_capacity() - cpu_stats->cpu_allocatable()) /
              (double)cpu_stats->cpu_capacity();
          cpu_stats->set_cpu_utilization(cpu_utilization);
          double mem_utilization = (resource_stats.mem_capacity() -
                                    resource_stats.mem_allocatable()) /
                                   (double)resource_stats.mem_capacity();
          resource_stats.set_mem_utilization(mem_utilization);
          double ephemeral_storage_utilization = (resource_stats.ephemeral_storage_capacity() -
                                    resource_stats.ephemeral_storage_allocatable()) /
                                   (double)resource_stats.ephemeral_storage_capacity();
          resource_stats.set_ephemeral_storage_utilization(ephemeral_storage_utilization);
          knowledge_base_->AddMachineSample(resource_stats);
        }
      }
      HandleTaskEviction(td_ptr, rs->mutable_descriptor());
    } else if (delta->type() == SchedulingDelta::MIGRATE) {
      HandleTaskMigration(td_ptr, rs->mutable_descriptor());
    } else {
      LOG(FATAL) << "Unhandled scheduling delta case";
    }
  }
  return num_scheduled;
}

void FlowScheduler::DeregisterResource(
    ResourceTopologyNodeDescriptor* rtnd_ptr) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  // Traverse the resource topology tree in order to evict tasks.
  DFSTraversePostOrderResourceProtobufTreeReturnRTND(
      rtnd_ptr,
      boost::bind(&FlowScheduler::HandleTasksFromDeregisteredResource,
                  this, _1));
  flow_graph_manager_->RemoveResourceTopology(
      rtnd_ptr->resource_desc(), &pus_removed_during_solver_run_);
  if (rtnd_ptr->parent_id().empty()) {
    resource_roots_.erase(rtnd_ptr);
  }
  EventDrivenScheduler::DeregisterResource(rtnd_ptr);
}

void FlowScheduler::HandleTasksFromDeregisteredResource(
    ResourceTopologyNodeDescriptor* rtnd_ptr) {
  ResourceID_t res_id = ResourceIDFromString(rtnd_ptr->resource_desc().uuid());
  vector<TaskID_t> tasks = BoundTasksForResource(res_id);
  ResourceDescriptor* rd_ptr = rtnd_ptr->mutable_resource_desc();
  for (auto& task_id : tasks) {
    TaskDescriptor* td_ptr = FindPtrOrNull(*task_map_, task_id);
    if (FLAGS_reschedule_tasks_upon_node_failure) {
      HandleTaskEviction(td_ptr, rd_ptr);
    } else {
      HandleTaskFailure(td_ptr);
    }
  }
}

void FlowScheduler::HandleJobCompletion(JobID_t job_id) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  // Job completed, so remove its nodes
  flow_graph_manager_->JobCompleted(job_id);
  // Call into superclass handler
  EventDrivenScheduler::HandleJobCompletion(job_id);
}

void FlowScheduler::HandleJobRemoval(JobID_t job_id) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  flow_graph_manager_->JobRemoved(job_id);
  JobDescriptor* jdp = FindOrNull(*job_map_, job_id);
  if (jdp) {
      affinity_job_to_deltas_.erase(jdp);
  }
  // Call into superclass handler
  EventDrivenScheduler::HandleJobRemoval(job_id);
}

void FlowScheduler::HandleTaskCompletion(TaskDescriptor* td_ptr,
                                         TaskFinalReport* report) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  bool task_in_graph = true;
  if (td_ptr->state() == TaskDescriptor::FAILED ||
      td_ptr->state() == TaskDescriptor::ABORTED) {
    // If the task is marked as failed/aborted then it has already been
    // removed from the flow network.
    task_in_graph = false;
  }
  // pod affinity/anti-affinity symmetry
  if (FLAGS_pod_affinity_antiaffinity_symmetry) {
    cost_model_->RemoveTaskFromTaskSymmetryMap(td_ptr);
  }
  // We first call into the superclass handler because it populates
  // the task report. The report might be used by the cost models.
  EventDrivenScheduler::HandleTaskCompletion(td_ptr, report);
  // We don't need to do any flow graph stuff for delegated tasks as
  // they are not currently represented in the flow graph.
  // Otherwise, we need to remove nodes, etc.
  if (td_ptr->delegated_from().empty() && task_in_graph) {
    uint64_t task_node_id = flow_graph_manager_->TaskCompleted(td_ptr->uid());
    tasks_completed_during_solver_run_.insert(task_node_id);
  }
}

void FlowScheduler::HandleTaskEviction(TaskDescriptor* td_ptr,
                                       ResourceDescriptor* rd_ptr) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  flow_graph_manager_->TaskEvicted(td_ptr->uid(),
                                   ResourceIDFromString(rd_ptr->uuid()));
  vector<TaskID_t>::iterator it =
                    find(affinity_antiaffinity_tasks_->begin(),
                    affinity_antiaffinity_tasks_->end(), td_ptr->uid());
  if (it == affinity_antiaffinity_tasks_->end()) {
     affinity_antiaffinity_tasks_->push_back(*it);
  }
  if (FLAGS_pod_affinity_antiaffinity_symmetry) {
    cost_model_->RemoveTaskFromTaskSymmetryMap(td_ptr);
  }
  EventDrivenScheduler::HandleTaskEviction(td_ptr, rd_ptr);
}

void FlowScheduler::HandleTaskFailure(TaskDescriptor* td_ptr) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  flow_graph_manager_->TaskFailed(td_ptr->uid());
  // pod affinity/anti-affinity symmetry
  if (FLAGS_pod_affinity_antiaffinity_symmetry) {
    cost_model_->RemoveTaskFromTaskSymmetryMap(td_ptr);
  }
  EventDrivenScheduler::HandleTaskFailure(td_ptr);
}

void FlowScheduler::HandleTaskFinalReport(const TaskFinalReport& report,
                                          TaskDescriptor* td_ptr) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  TaskID_t task_id = td_ptr->uid();
  vector<EquivClass_t>* equiv_classes =
    cost_model_->GetTaskEquivClasses(task_id);
  CHECK_NOTNULL(equiv_classes);
  knowledge_base_->ProcessTaskFinalReport(*equiv_classes, report);
  delete equiv_classes;
  // NOTE: We should remove the task from the cost model in TaskCompleted.
  // However, we cannot do that because in this method we need the
  // task's equivalence classes.
  cost_model_->RemoveTask(task_id);
  EventDrivenScheduler::HandleTaskFinalReport(report, td_ptr);
}

void FlowScheduler::HandleTaskMigration(TaskDescriptor* td_ptr,
                                        ResourceDescriptor* rd_ptr) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  TaskID_t task_id = td_ptr->uid();
  // Get the old resource id before we call EventDrivenScheduler.
  // Otherwise, we would end up getting the new resource id.
  ResourceID_t* old_res_id_ptr = FindOrNull(task_bindings_, task_id);
  CHECK_NOTNULL(old_res_id_ptr);
  ResourceID_t old_res_id = *old_res_id_ptr;
  // XXX(ionel): HACK! We update scheduled_to_resource field here
  // and in the EventDrivenScheduler. We update it here because
  // TaskMigrated first calls TaskEvict and then TaskSchedule.
  // TaskSchedule requires scheduled_to_resource to be up to date.
  // Hence, we have to set it before we call the method.
  td_ptr->set_scheduled_to_resource(rd_ptr->uuid());
  flow_graph_manager_->TaskMigrated(task_id, old_res_id,
                                    ResourceIDFromString(rd_ptr->uuid()));
  EventDrivenScheduler::HandleTaskMigration(td_ptr, rd_ptr);
}

void FlowScheduler::HandleTaskPlacement(TaskDescriptor* td_ptr,
                                        ResourceDescriptor* rd_ptr) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  td_ptr->set_scheduled_to_resource(rd_ptr->uuid());
  flow_graph_manager_->TaskScheduled(td_ptr->uid(),
                                     ResourceIDFromString(rd_ptr->uuid()));
  // Pod affinity/anti-affinity
  if (td_ptr->has_affinity() && (td_ptr->affinity().has_pod_affinity() ||
                                 td_ptr->affinity().has_pod_anti_affinity())) {
    vector<TaskID_t>::iterator it =
        find(affinity_antiaffinity_tasks_->begin(),
             affinity_antiaffinity_tasks_->end(), td_ptr->uid());
    if (it != affinity_antiaffinity_tasks_->end()) {
      affinity_antiaffinity_tasks_->erase(it);
    }
    // pod affinity/anti-affinity symmetry
    if (FLAGS_pod_affinity_antiaffinity_symmetry) {
      cost_model_->UpdateResourceToTaskSymmetryMap(ResourceIDFromString(rd_ptr->uuid()), td_ptr->uid());
    }
  }
  EventDrivenScheduler::HandleTaskPlacement(td_ptr, rd_ptr);
}

void FlowScheduler::HandleTaskRemoval(TaskDescriptor* td_ptr) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  flow_graph_manager_->TaskRemoved(td_ptr->uid());
  // pod affinity/anti-affinity symmetry
  if (FLAGS_pod_affinity_antiaffinity_symmetry) {
    cost_model_->RemoveTaskFromTaskSymmetryMap(td_ptr);
  }
  EventDrivenScheduler::HandleTaskRemoval(td_ptr);
}

void FlowScheduler::KillRunningTask(TaskID_t task_id,
                                    TaskKillMessage::TaskKillReason reason) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  flow_graph_manager_->TaskKilled(task_id);
  EventDrivenScheduler::KillRunningTask(task_id, reason);
}

void FlowScheduler::LogDebugCostModel() {
  string csv_log;
  spf(&csv_log, "%s/cost_model_%d.csv", FLAGS_debug_output_dir.c_str(),
      solver_dispatcher_->seq_num());
  FILE* csv_log_file = fopen(csv_log.c_str(), "w");
  CHECK_NOTNULL(csv_log_file);
  string debug_info = cost_model_->DebugInfoCSV();
  fputs(debug_info.c_str(), csv_log_file);
  CHECK_EQ(fclose(csv_log_file), 0);
}

void FlowScheduler::PopulateSchedulerResourceUI(
    ResourceID_t res_id,
    TemplateDictionary* dict) const {
}

void FlowScheduler::PopulateSchedulerTaskUI(TaskID_t task_id,
                                            TemplateDictionary* dict) const {
  vector<EquivClass_t>* equiv_classes =
    cost_model_->GetTaskEquivClasses(task_id);
  if (equiv_classes) {
    for (vector<EquivClass_t>::iterator it = equiv_classes->begin();
         it != equiv_classes->end(); ++it) {
      TemplateDictionary* tec_dict = dict->AddSectionDictionary("TASK_TECS");
      tec_dict->SetFormattedValue("TASK_TEC", "%ju", *it);
    }
  }
  delete equiv_classes;
}

uint64_t FlowScheduler::ScheduleAllJobs(SchedulerStats* scheduler_stats) {
  return ScheduleAllJobs(scheduler_stats, NULL);
}

uint64_t FlowScheduler::ScheduleAllQueueJobs(SchedulerStats* scheduler_stats,
                                             vector<SchedulingDelta>* deltas) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  queue_based_schedule = true;
  uint64_t num_scheduled_tasks = ScheduleAllJobs(scheduler_stats, deltas);
  queue_based_schedule = false;
  return num_scheduled_tasks;
}

uint64_t FlowScheduler::ScheduleAllJobs(SchedulerStats* scheduler_stats,
                                        vector<SchedulingDelta>* deltas) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  vector<JobDescriptor*> jobs;
  //Pod affinity/anti-affinity
  one_task_runnable = false;
  for (auto& job_id_jd : jobs_to_schedule_) {
    const TaskDescriptor& td = job_id_jd.second->root_task();
    if (queue_based_schedule) {
      if (!(td.has_affinity() && (td.affinity().has_pod_affinity() ||
          td.affinity().has_pod_anti_affinity()))) {
        continue;
      }
    } else {
      if ((td.has_affinity() && (td.affinity().has_pod_affinity() ||
        td.affinity().has_pod_anti_affinity()))) {
        continue;
      }
    }
    if (ComputeRunnableTasksForJob(job_id_jd.second).size() > 0) {
      jobs.push_back(job_id_jd.second);
    }
  }
  uint64_t num_scheduled_tasks = ScheduleJobs(jobs, scheduler_stats, deltas);
  //Pod affinity/anti-affinity
  one_task_runnable = false;
  return num_scheduled_tasks;
}

uint64_t FlowScheduler::ScheduleJob(JobDescriptor* jd_ptr,
                                    SchedulerStats* scheduler_stats) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  LOG(INFO) << "START SCHEDULING (via " << jd_ptr->uuid() << ")";
  LOG(WARNING) << "This way of scheduling a job is slow in the flow scheduler! "
               << "Consider using ScheduleAllJobs() instead.";
  vector<JobDescriptor*> jobs_to_schedule {jd_ptr};
  return ScheduleJobs(jobs_to_schedule, scheduler_stats);
}

uint64_t FlowScheduler::ScheduleJobs(const vector<JobDescriptor*>& jd_ptr_vect,
                                     SchedulerStats* scheduler_stats,
                                     vector<SchedulingDelta>* deltas) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  CHECK_NOTNULL(scheduler_stats);
  uint64_t num_scheduled_tasks = 0;
  boost::timer::cpu_timer total_scheduler_timer;
  vector<JobDescriptor*> jds_with_runnables;
  for (auto& jd_ptr : jd_ptr_vect) {
    // Check if we have any runnable tasks in this job
    const unordered_set<TaskID_t> runnable_tasks =
      ComputeRunnableTasksForJob(jd_ptr);
    if (runnable_tasks.size() > 0) {
      jds_with_runnables.push_back(jd_ptr);
    }
  }
  // XXX(ionel): HACK! We should only run the scheduler when we have
  // runnable jobs. However, we also run the scheduler when we've
  // set the flowlessly_flip_algorithms flag in order to speed up
  // simulators and make sure different simulations are synchronous.
  if (jds_with_runnables.size() > 0 ||
      (FLAGS_flowlessly_flip_algorithms &&
       time_manager_->GetCurrentTimestamp() >= SIMULATION_START_TIME)) {
    // First, we update the cost model's resource topology statistics
    // (e.g. based on machine load and prior decisions); these need to be
    // known before AddOrUpdateJobNodes is invoked below, as it may add arcs
    // depending on these metrics.
    UpdateCostModelResourceStats();
    if (FLAGS_gather_unscheduled_tasks)  {
      // Clear unscheduled tasks related maps and sets.
      cost_model_->ClearUnscheduledTasksData();
    }
    flow_graph_manager_->AddOrUpdateJobNodes(jds_with_runnables);
    num_scheduled_tasks += RunSchedulingIteration(scheduler_stats, deltas, &jds_with_runnables);
    VLOG(1) << "STOP SCHEDULING, placed " << num_scheduled_tasks << " tasks";
    // If we have cost model debug logging turned on, write some debugging
    // information now.
    if (FLAGS_debug_cost_model) {
      LogDebugCostModel();
    }
    // We reset the DIMACS stats here because all the graph changes we make
    // from now on are going to be included in the next scheduler run.
    DIMACSChangeStats current_run_dimacs_stats = *dimacs_stats_;
    dimacs_stats_->ResetStats();
    scheduler_stats->total_runtime_ =
      static_cast<uint64_t>(total_scheduler_timer.elapsed().wall) /
      NANOSECONDS_IN_MICROSECOND;
    trace_generator_->SchedulerRun(*scheduler_stats, current_run_dimacs_stats);
  }
  return num_scheduled_tasks;
}

void FlowScheduler::RegisterResource(ResourceTopologyNodeDescriptor* rtnd_ptr,
                                     bool local,
                                     bool simulated) {
  boost::lock_guard<boost::recursive_mutex> lock(scheduling_lock_);
  EventDrivenScheduler::RegisterResource(rtnd_ptr, local, simulated);
  flow_graph_manager_->AddResourceTopology(rtnd_ptr);
  if (rtnd_ptr->parent_id().empty()) {
    resource_roots_.insert(rtnd_ptr);
  }
}

uint64_t FlowScheduler::RunSchedulingIteration(
    SchedulerStats* scheduler_stats,
    vector<SchedulingDelta>* deltas_output, vector<JobDescriptor*>* job_vector) {
  // If it's time to revisit time-dependent costs, do so now, just before
  // we run the solver.
  uint64_t cur_time = time_manager_->GetCurrentTimestamp();
  if (last_updated_time_dependent_costs_ <= (cur_time -
      static_cast<uint64_t>(FLAGS_time_dependent_cost_update_frequency))) {
    // First collect all non-finished jobs
    // TODO(malte): this can be removed when we've factored archived tasks
    // and jobs out of the job_map_ into separate data structures.
    // (cf. issue #24).
    /*
    vector<JobDescriptor*> job_vec;
    for (auto it = job_map_->begin();
         it != job_map_->end();
         ++it) {
      // We only need to reconsider this job if it is still active
      if (it->second.state() != JobDescriptor::COMPLETED &&
          it->second.state() != JobDescriptor::FAILED &&
          it->second.state() != JobDescriptor::ABORTED) {
        job_vec.push_back(&it->second);
      }
    }
    */
    // This will re-visit all jobs and update their time-dependent costs
    // Changed above code to revisit only jobs from job_vector not from
    // job_map_ i.e, jobs with pod affinty and pod anti-affinity are handled
    // in sepearte scheduling round even for time dependent costs update.
    // Jobs with pod affinty/anti-affinty are scheduled one task at a time
    // in a single scheduling round, whereas for other jobs tasks are
    // scheduled in a batch.
    VLOG(1) << "Flow scheduler updating time-dependent costs.";
    vector<JobDescriptor*> job_vec;
    for (auto it = (*job_vector).begin();
         it != (*job_vector).end();
         ++it) {
      // We only need to reconsider this job if it is still active
      if ((*it)->state() != JobDescriptor::COMPLETED &&
          (*it)->state() != JobDescriptor::FAILED &&
          (*it)->state() != JobDescriptor::ABORTED) {
        job_vec.push_back(*it);
      }
    }
    if (FLAGS_gather_unscheduled_tasks)  {
      // Clear unscheduled tasks related maps and sets.
      cost_model_->ClearUnscheduledTasksData();
    }
    flow_graph_manager_->UpdateTimeDependentCosts(job_vec);
    last_updated_time_dependent_costs_ = cur_time;
  }
  if (solver_run_cnt_ % FLAGS_purge_unconnected_ec_frequency == 0) {
    // Periodically remove EC nodes without incoming arcs.
    flow_graph_manager_->PurgeUnconnectedEquivClassNodes();
  }
  pus_removed_during_solver_run_.clear();
  tasks_completed_during_solver_run_.clear();
  uint64_t scheduler_start_timestamp = time_manager_->GetCurrentTimestamp();
  // Run the flow solver! This is where all the juicy goodness happens :)
  multimap<uint64_t, uint64_t>* task_mappings;
  if (!queue_based_schedule) {
    task_mappings = solver_dispatcher_->Run(scheduler_stats);
  } else {
      string id = ((*job_vector)[0])->uuid();
      TaskID_t single_task_id = *(runnable_tasks_[JobIDFromString(id)].begin());
      // Single task that needs to scheduled.
      task_to_be_scheduled_ = single_task_id;
      pair<TaskID_t, ResourceID_t> single_delta =
        solver_dispatcher_->RunSimpleSolverForSingleTask(scheduler_stats,
                                                         single_task_id);
      task_mappings =
        flow_graph_manager_->PopulateTaskMappingsForSimpleSolver(&task_bindings_,
                                                                 single_delta);
  }
  solver_run_cnt_++;
  CHECK_LE(scheduler_stats->scheduler_runtime_, FLAGS_max_solver_runtime)
    << "Solver took longer than limit of "
    << scheduler_stats->scheduler_runtime_;
  // Play all the simulation events that happened while the solver was running.
  if (event_notifier_) {
    if (solver_run_cnt_ == 1) {
      // On the first run, we pretend that the solver took no time. This is in
      // order to avoid a long initial run that sets up the cluster state
      // from having a knock-on effect on subsequent runs.
      // (This matters most for simulation mode.)
      event_notifier_->OnSchedulingDecisionsCompletion(
         scheduler_start_timestamp, 0);
    } else {
      if (FLAGS_solver_runtime_accounting_mode == "algorithm") {
        if (FLAGS_flow_scheduling_solver == "cs2") {
          // CS2 doesn't export algorithm runtime. We fallback to solver mode.
          event_notifier_->OnSchedulingDecisionsCompletion(
              scheduler_start_timestamp, scheduler_stats->scheduler_runtime_);
        } else {
          event_notifier_->OnSchedulingDecisionsCompletion(
              scheduler_start_timestamp, scheduler_stats->algorithm_runtime_);
        }
      } else if (FLAGS_solver_runtime_accounting_mode == "solver") {
        event_notifier_->OnSchedulingDecisionsCompletion(
           scheduler_start_timestamp, scheduler_stats->scheduler_runtime_);
      } else if (FLAGS_solver_runtime_accounting_mode == "firmament") {
        event_notifier_->OnSchedulingDecisionsCompletion(
           scheduler_start_timestamp, scheduler_stats->total_runtime_);
      } else {
        LOG(FATAL) << "Unexpected accounting mode: "
                   << FLAGS_solver_runtime_accounting_mode;
      }
    }
  }
  // Solver's done, let's post-process the results.
  multimap<uint64_t, uint64_t>::iterator it;
  vector<SchedulingDelta*> deltas;
  // We first generate the deltas for the preempted tasks in a separate step.
  // Otherwise, we would have to maintain for every ResourceDescriptor the
  // current_running_tasks field which would be expensive because
  // RepeatedFields don't have any efficient remove element method.
  flow_graph_manager_->SchedulingDeltasForPreemptedTasks(*task_mappings,
                                                         resource_map_,
                                                         &deltas);
  for (it = task_mappings->begin(); it != task_mappings->end(); it++) {
    if (tasks_completed_during_solver_run_.find(it->first) !=
        tasks_completed_during_solver_run_.end()) {
      // Ignore the task because it has already completed while the solver
      // was running.
      VLOG(1) << "Task with node id: " << it->first
              << " completed while the solver was running";
      continue;
    }
    if (pus_removed_during_solver_run_.find(it->second) !=
        pus_removed_during_solver_run_.end()) {
      // We can't place a task on this PU because the PU has been removed
      // while the solver was running. We will reconsider the task in the
      // next solver run.
      VLOG(1) << "PU with node id: " << it->second
              << " was removed while the solver was running";
      continue;
    }
    VLOG(2) << "Bind " << it->first << " to " << it->second << endl;
    flow_graph_manager_->NodeBindingToSchedulingDeltas(it->first, it->second,
                                                       &task_bindings_,
                                                       &deltas);
    const FlowGraphNode& task_node =
        flow_graph_manager_->node_for_node_id(it->first);
    CHECK(task_node.IsTaskNode());
    CHECK_NOTNULL(task_node.td_ptr_);
    TaskDescriptor* td_ptr = task_node.td_ptr_;
    JobDescriptor* jd = FindOrNull(*job_map_,
                                   JobIDFromString(td_ptr->job_id()));
    CHECK_NOTNULL(jd);
    if (jd->is_gang_scheduling_job()
        && affinity_delta_tasks.find(td_ptr->uid())
                                  == affinity_delta_tasks.end()) {
      uint64_t scheduled_tasks_count = jd->scheduled_tasks_count();
      jd->set_scheduled_tasks_count(++scheduled_tasks_count);
    }
  }
  // Freeing the mappings because they're not used below.
  delete task_mappings;

  // Move the time to solver_start_time + solver_run_time if this is not
  // the first run of a simulation.
  if (time_manager_->GetCurrentTimestamp() != 0 && solver_run_cnt_ > 1) {
    // Set the current timestamp to the timestamp of the end of the scheduling
    // round. Thus, we make sure that all the changes applied as a result of
    // scheduling have a timestamp equal to the end of the scheduling iteration.
    if (FLAGS_solver_runtime_accounting_mode == "algorithm") {
      if (FLAGS_flow_scheduling_solver == "cs2") {
        // CS2 doesn't export algorithm runtime. We fallback to solver mode.
        time_manager_->UpdateCurrentTimestamp(
            scheduler_start_timestamp + scheduler_stats->scheduler_runtime_);
      } else {
        time_manager_->UpdateCurrentTimestamp(
            scheduler_start_timestamp + scheduler_stats->algorithm_runtime_);

      }
    } else if (FLAGS_solver_runtime_accounting_mode == "solver") {
      time_manager_->UpdateCurrentTimestamp(
          scheduler_start_timestamp + scheduler_stats->scheduler_runtime_);
    } else if (FLAGS_solver_runtime_accounting_mode == "firmament") {
      time_manager_->UpdateCurrentTimestamp(
          scheduler_start_timestamp + scheduler_stats->total_runtime_);
    } else {
      LOG(FATAL) << "Unexpected accounting mode: "
                 << FLAGS_solver_runtime_accounting_mode;
    }
  }
  uint64_t num_scheduled = ApplySchedulingDeltas(deltas);
  if (deltas_output) {
    for (auto& delta : deltas) {
      if (delta->type() == SchedulingDelta::NOOP) {
        continue;
      }
      deltas_output->push_back(*delta);
    }
  }
  // Makes sure the deltas get correctly freed.
  deltas.clear();
  time_manager_->UpdateCurrentTimestamp(scheduler_start_timestamp);
  if (FLAGS_update_resource_topology_capacities) {
    for (auto& rtnd_ptr : resource_roots_) {
      flow_graph_manager_->UpdateResourceTopology(rtnd_ptr);
    }
  }
  return num_scheduled;
}

void FlowScheduler::UpdateCostModelResourceStats() {
  VLOG(2) << "Updating resource statistics in flow graph";
  flow_graph_manager_->ComputeTopologyStatistics(
      flow_graph_manager_->sink_node(),
      boost::bind(&CostModelInterface::PrepareStats, cost_model_, _1),
      boost::bind(&CostModelInterface::GatherStats, cost_model_, _1, _2),
      boost::bind(&CostModelInterface::UpdateStats, cost_model_, _1, _2));
}

void FlowScheduler::AddKnowledgeBaseResourceStats(TaskDescriptor* td_ptr,
                                               ResourceStatus* rs) {
  ResourceStats resource_stats;
  CpuStats* cpu_stats = resource_stats.add_cpus_stats();
  bool have_sample = knowledge_base_->GetLatestStatsForMachine(
      ResourceIDFromString(rs->mutable_topology_node()->parent_id()),
      &resource_stats);
  if (have_sample) {
    cpu_stats->set_cpu_allocatable(
        cpu_stats->cpu_allocatable() +
        td_ptr->resource_request().cpu_cores());
    resource_stats.set_mem_allocatable(
        resource_stats.mem_allocatable() +
        td_ptr->resource_request().ram_cap());
    resource_stats.set_ephemeral_storage_allocatable(
        resource_stats.ephemeral_storage_allocatable() +
        td_ptr->resource_request().ephemeral_storage());
    double cpu_utilization =
        (cpu_stats->cpu_capacity() - cpu_stats->cpu_allocatable()) /
        (double)cpu_stats->cpu_capacity();
    cpu_stats->set_cpu_utilization(cpu_utilization);
    double mem_utilization = (resource_stats.mem_capacity() -
                              resource_stats.mem_allocatable()) /
                             (double)resource_stats.mem_capacity();
    resource_stats.set_mem_utilization(mem_utilization);
    double ephemeral_storage_utilization = (resource_stats.ephemeral_storage_capacity() -
                              resource_stats.ephemeral_storage_allocatable()) /
                             (double)resource_stats.ephemeral_storage_capacity();
    resource_stats.set_ephemeral_storage_utilization(ephemeral_storage_utilization);
    knowledge_base_->AddMachineSample(resource_stats);
  }
}

void FlowScheduler::UpdateGangSchedulingDeltas(
                    SchedulerStats* scheduler_stats,
                    vector<SchedulingDelta>* deltas_output,
                    vector<uint64_t>* unscheduled_normal_tasks,
                    unordered_set<uint64_t>* unscheduled_affinity_tasks_set,
                    vector<uint64_t>* unscheduled_affinity_tasks) {
  // update batch schedule deltas
  for (auto job_ptr : delta_jobs) {
    TaskDescriptor rtd = job_ptr->root_task();
    for (auto td : rtd.spawned()) {
      vector<uint64_t>::iterator it = find(unscheduled_normal_tasks->begin(),
                                      unscheduled_normal_tasks->end(),
                                      td.uid());
      if (it == unscheduled_normal_tasks->end()) {
        unscheduled_normal_tasks->push_back(td.uid());
      }
    }
    delta_jobs.clear();

    vector<uint64_t>::iterator rit = find(unscheduled_normal_tasks->begin(),
                                     unscheduled_normal_tasks->end(),
                                     rtd.uid());
    if (rit == unscheduled_normal_tasks->end()) {
      unscheduled_normal_tasks->push_back(rtd.uid());
    }
  }

  // update queue schedule deltas
  for (auto it = affinity_job_to_deltas_.begin();
            it != affinity_job_to_deltas_.end(); ++it) {
    JobDescriptor* jd_ptr = it->first;
    TaskDescriptor root_td = jd_ptr->root_task();
    if (!it->second.size()) {
      for (auto td : root_td.spawned()) {
        if (td.state() != TaskDescriptor::RUNNING) {
          unscheduled_affinity_tasks_set->insert(td.uid());
          unscheduled_affinity_tasks->push_back(td.uid());
        }
      }
      if (root_td.state() != TaskDescriptor::RUNNING) {
        unscheduled_affinity_tasks_set->insert(root_td.uid());
        unscheduled_affinity_tasks->push_back(root_td.uid());
      }
      continue;
    }    
    if (jd_ptr->scheduled_tasks_count() < jd_ptr->min_number_of_tasks()) {
      for (auto delta : it->second) {
        TaskDescriptor* td_ptr = FindPtrOrNull(*task_map_, delta.task_id());
        ResourceID_t res_id = ResourceIDFromString(delta.resource_id());
        ResourceStatus* rs = FindPtrOrNull(*resource_map_, res_id);
        CHECK_NOTNULL(td_ptr);
        CHECK_NOTNULL(rs);
        if (FLAGS_resource_stats_update_based_on_resource_reservation) {
          AddKnowledgeBaseResourceStats(td_ptr, rs);
        }
        HandleTaskEviction(td_ptr, rs->mutable_descriptor());
        td_ptr->set_state(TaskDescriptor::CREATED);
        td_ptr->clear_scheduled_to_resource();
        JobID_t job_id = JobIDFromString(jd_ptr->uuid());
        unordered_set<TaskID_t>* runnables_for_job =
          FindOrNull(runnable_tasks_, job_id);
        if (runnables_for_job) {
          runnables_for_job->erase(delta.task_id());
        }
        vector<SchedulingDelta>::iterator it =
              find_if(deltas_output->begin(), deltas_output->end(),
              [&](SchedulingDelta& d){return (d.task_id() == delta.task_id());});
        if (it != deltas_output->end()) {
          deltas_output->erase(it);
        }
      }
      for (auto td : root_td.spawned()) {
        unscheduled_affinity_tasks_set->insert(td.uid());
        unscheduled_affinity_tasks->push_back(td.uid());
      }
      unscheduled_affinity_tasks_set->insert(root_td.uid());
      unscheduled_affinity_tasks->push_back(root_td.uid());
    } else {
      for (auto td : root_td.spawned()) {
        if (td.state() != TaskDescriptor::RUNNING) {
          unscheduled_affinity_tasks_set->insert(td.uid());
          unscheduled_affinity_tasks->push_back(td.uid());
        }
      }
      if (root_td.state() != TaskDescriptor::RUNNING) {
        unscheduled_affinity_tasks_set->insert(root_td.uid());
        unscheduled_affinity_tasks->push_back(root_td.uid());
      }
    }
    jd_ptr->set_scheduled_tasks_count(0);
    it->second.clear();
  }
  affinity_delta_tasks.clear();
}

}  // namespace scheduler
}  // namespace firmament
