#include "training/graph_group.h"

namespace marian {

GraphGroup::GraphGroup(Ptr<Options> options, const std::vector<DeviceId> devices)
  : options_(options),
    devices_(devices) {
  if(options_->hasAndNotEmpty("cost-scaling")) {
    auto vcs = options_->get<std::vector<std::string>>("cost-scaling");
    costScale_ = true;
    float costExponent = std::stof(vcs[0]);
    costScaleFactor_ = std::pow(2.0f, costExponent);
    costScaleFreq_ = std::stoul(vcs[1]);
    costScaleMultiplier_ = std::stof(vcs[2]);
    nanTolerance_ = std::stof(vcs[3]);

    LOG_ONCE(info,
              "Training with cost scaling - factor: 2^{} = {}, frequency: {}, multiplier: {}, tolerance: {}",
              costExponent,
              costScaleFactor_,
              costScaleFreq_,
              costScaleMultiplier_,
              nanTolerance_);
  }
}

GraphGroup::GraphGroup(Ptr<Options> options) 
  : GraphGroup(options, Config::getDevices(options)) {}

// increase cost-scaling factor if no NaN has been detected for a
// given number of iterations. Usually we increase by 2 which adds
// one more bit for precision.
void GraphGroup::increaseCostScaleFactor() {
  if(!costScale_)
    return;

  noNanSeen_++;

  float nanPercent = noNanSeen_ == 0 ? 1.f : (float)nanSeen_ / (float)noNanSeen_;

  if(noNanSeen_ % costScaleFreq_ == 0) {
    costScaleFactor_ *= costScaleMultiplier_;
    LOG(info,
        "NaN/Inf percentage {:.2f} after {} updates. Increasing cost-scaling factor to {}",
        nanPercent,
        noNanSeen_,
        costScaleFactor_);
  }
}

// call when a NaN was seen to decrease cost-scaling factor
void GraphGroup::decreaseCostScaleFactor() {
  if(!costScale_)
    return;

  nanSeen_++;
  float nanPercent = noNanSeen_ == 0 ? 1.f : (float)nanSeen_ / (float)noNanSeen_;
  if(nanPercent > nanTolerance_) {
    costScaleFactor_ /= costScaleMultiplier_;
    LOG(warn,
        "NaN/Inf percentage {:.2f} in gradients, skipping update, reducing cost-scaling factor to {}",
        nanPercent,
        costScaleFactor_);

    noNanSeen_ = 0;
    nanSeen_ = 0;
  }
}

void GraphGroup::load(const OptimizerBase::ScatterStateFunc& scatterFn) {
  if(!options_->get<bool>("no-reload")) {
    std::string name = options_->get<std::string>("model");

    if(filesystem::exists(name)) {
      if(scheduler_)
        scheduler_->load(name);

      std::string nameGraph = name;
      size_t i = 0;
      for(auto graph : graphs_)
        models_[i++]->load(graph, nameGraph); // we just load it N times from disk (it'll be in disk cache after the first)

      restoreCheckpoint(scatterFn);

      LOG(info, "[training] Model reloaded from {}", name);
    } else if(options_->hasAndNotEmpty("pretrained-model")) {
      std::string nameInit = options_->get<std::string>("pretrained-model");
      LOG(info,
          "[training] Initializing model weights with pre-trained model {}",
          nameInit);
    
      size_t i = 0;
      for(auto graph : graphs_)
        models_[i++]->load(graph, nameInit, false);
    }
  }
}

void GraphGroup::restoreCheckpoint(const OptimizerBase::ScatterStateFunc& scatterFn) {
  std::string name = options_->get<std::string>("model");
  
  // @TODO: probably we want to have the list of DeviceIds as an attribute
  std::vector<Ptr<Backend>> backends;
  for(auto graph : graphs_)
    backends.push_back(graph->getBackend());        
  optimizerShards_[0]->load(name + ".optimizer.npz", optimizerShards_, backends, scatterFn);
}

void GraphGroup::save(bool isFinal,
                      const std::function<void()>& distributeParamtersFn,
                      const OptimizerBase::GatherStateFunc& gatherOptimizerStateFn,
                      bool isMainProcess) { 
  
  barrier(); // (for better grouping of log messages)
  
  if(isMainProcess) { // only save from one MPI process
    // bring the smoothed model in
    // Note that it is sharded. For multi-node, it is sharded over multiple machines, so this is a network access.
    // Also note that the swap must run on all MPI processes concurrently, although only one actually validates.
    swapWithSmoothed(graphs_, optimizerShards_, distributeParamtersFn);
    
    // do final validation
    if(isFinal && scheduler_)
      scheduler_->validate(graphs_, isFinal);
    
    barrier();// (for better grouping of log messages)

    // save main model file
    saveModel(isFinal);  // if not overwrite then save a copy with number of updates in the model pathname


    swapWithOriginal(graphs_, optimizerShards_, distributeParamtersFn);
  }
  barrier(); // (for better grouping of log messages)

  saveCheckpoint(gatherOptimizerStateFn, isMainProcess);

  barrier(); // (for better grouping of log messages)
}

void GraphGroup::saveModel(bool isFinal) {
  std::string name = options_->get<std::string>("model");
  
  if(options_->get<bool>("overwrite")) {
    models_[0]->save(graphs_[0], name, /*saveTranslatorConfig=*/true);
    // save scheduler-related state
    if(scheduler_)
      scheduler_->save(name);
  } else {
    if(!isFinal) { // save a model with iteration number
      std::string numberOfBatches
          = scheduler_ ? std::to_string(scheduler_->numberOfBatches())
                        : "unknown";
      std::string nameOverwrite = name;
      nameOverwrite.replace(name.size() - 4, 4, ".iter" + numberOfBatches + ".npz");
      models_[0]->save(graphs_[0], nameOverwrite);
    }

    models_[0]->save(graphs_[0], name, /*saveTranslatorConfig=*/true);
    
    // save scheduler-related state
    if(scheduler_)
      scheduler_->save(name);
  }
}

void GraphGroup::saveCheckpoint(const OptimizerBase::GatherStateFunc& gatherFn, 
                                bool isMainProcess) {
  // @TODO: this should do more, also numer checkpoints,
  // contain full model copy etc.
  // We might consider making GraphGroup the main checkpointer 
  // instead of OptimizerBase as it is now. 
  // This should be easy with the IoItem interface
  std::string name = options_->get<std::string>("model");

  optimizerShards_[0]->save(name + ".optimizer.npz", 
                            optimizerShards_, 
                            gatherFn, 
                            isMainProcess);
}

void GraphGroup::swapWithSmoothed(const std::vector<Ptr<ExpressionGraph>>& graphs,
                                  const std::vector<Ptr<OptimizerBase>>& opts,
                                  const std::function<void()>& distribute) {
  ABORT_IF(graphs.size() != opts.size(), "Number of graphs and optimizers has to be equal ({} != {})", graphs.size() != opts.size());
  for(size_t i = 0; i < graphs.size(); ++i)
    opts[i]->swapWithSmoothed(graphs[i], i, graphs.size(), /*swapAvg=*/true);
  distribute();
}

void GraphGroup::swapWithOriginal(const std::vector<Ptr<ExpressionGraph>>& graphs,
                                  const std::vector<Ptr<OptimizerBase>>& opts,
                                  const std::function<void()>& distribute) {
  ABORT_IF(graphs.size() != opts.size(), "Number of graphs and optimizers has to be equal ({} != {})", graphs.size() != opts.size());
  for(size_t i = 0; i < graphs.size(); ++i)
    opts[i]->swapWithSmoothed(graphs[i], i, graphs.size(), /*swapAvg=*/false);
  distribute();
}

void GraphGroup::validate() {
  ABORT_IF(finalized_, "Training has already finished.");
}

void GraphGroup::finalize() {
  finalized_ = true;
}

/**
 * Determine maximal batch size that can fit into the given workspace
 * so that reallocation does not happen. Rather adjust the batch size
 * based on the stastistics collected here. Activated with
 * `--mini-batch-fit`.
 * In a multi-GPU scenario, the first GPU is used to determine the size.
 * The actual allowed size is then determined by multiplying it with the
 * number of devices, which is passed in as the 'multiplier'.
 */
// @TODO: Can this be made const? It seems wrong to have a stateful method that still returns a result.
Ptr<data::BatchStats> GraphGroup::collectStats(Ptr<ExpressionGraph> graph,
                                               Ptr<models::ModelBase> model,
                                               const std::vector<Ptr<Vocab>>& vocabs,
                                               double multiplier) {
  // this runs with fake values, we do not care for overflow/underflow
  bool throwNan = graph->getThrowNan();
  graph->setThrowNan(false);

  auto stats = New<data::BatchStats>();

  size_t numFiles = options_->get<std::vector<std::string>>("train-sets").size();

  // Initialize first batch to step size
  size_t first = options_->get<size_t>("mini-batch-fit-step");

  // Increase batch size and sentence length by this step size
  size_t step = options_->get<size_t>("mini-batch-fit-step");

  size_t maxLength = options_->get<size_t>("max-length");
  maxLength = (size_t)(std::ceil(maxLength / (float)step) * step);

  // this should be only one class label per line on input, hence restricting length to 1
  std::vector<size_t> localMaxes(numFiles, maxLength);
  auto inputTypes = options_->get<std::vector<std::string>>("input-types", {});
  for(int i = 0; i < inputTypes.size(); ++i)
    if(inputTypes[i] == "class")
      localMaxes[i] = 1;

  size_t maxBatch = 512;
  bool fits = true;
  while(fits) {
    std::vector<size_t> lengths(numFiles, first);

    for(int j = 0; j < lengths.size(); ++j) // apply length restrictions
      lengths[j] = std::min(lengths[j], localMaxes[j]);

    auto batch = data::CorpusBatch::fakeBatch(lengths, vocabs, maxBatch, options_);
    auto loss = model->build(graph, batch);
    fits = graph->fits();
    if(fits)
      maxBatch *= 2;
  }

  // Do a binary search for maxmimum batch size that fits into given workspace memory
  // for a tested sentence length.
  for(size_t i = step; i <= maxLength; i += step) {
    size_t start = 1;
    size_t end = maxBatch;

    std::vector<size_t> lengths(numFiles, i);
    for(int j = 0; j < lengths.size(); ++j)  // apply length restrictions
      lengths[j] = std::min(lengths[j], localMaxes[j]);
    fits = true;

    do {
      size_t current = (start + end) / 2;
      auto batch = data::CorpusBatch::fakeBatch(lengths, vocabs, current, options_);
      auto loss = model->build(graph, batch);
      fits = graph->fits();

      LOG(debug, "[batching] length: {} - size: {} - fits: {}", lengths[0], current, fits);

      if(fits) {
        stats->add(batch, multiplier);
        start = current + 1;
      } else {
        end = current - 1;
      }
    } while(end >= start);

    maxBatch = start;
  }

  // set back to original value for aborting on NaN or Inf
  graph->setThrowNan(throwNan);
  return stats;
}

void GraphGroup::setTypicalTrgBatchWords(size_t typicalTrgBatchWords) { // needed for dynamic MB scaling
  typicalTrgBatchWords_ = typicalTrgBatchWords;
}

}