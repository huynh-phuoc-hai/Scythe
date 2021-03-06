/**
    classification_forest.cpp
    Classification forests

    @author Antoine Passemiers
    @version 1.0 12/04/2017
*/

#include "classification_forest.hpp"

ClassificationForest::ClassificationForest
    (ForestConfig* config, size_t n_instances, size_t n_features) :
    Forest(n_instances, n_features) {
    Forest::config = *config;
    Forest::base_tree_config.task = gbdf::CLASSIFICATION_TASK;
    Forest::base_tree_config.nan_value = config->nan_value;
    Forest::base_tree_config.n_classes = config->n_classes;
    Forest::base_tree_config.is_incremental = false;
    Forest::base_tree_config.min_threshold = 1e-06;
    Forest::base_tree_config.max_height = config->max_depth;
    Forest::base_tree_config.max_nodes = config->max_n_nodes;
    Forest::base_tree_config.partitioning = gbdf::PERCENTILE_PARTITIONING;
    Forest::grad_trees_config = Forest::base_tree_config;
    Forest::grad_trees_config.task = gbdf::REGRESSION_TASK;
    // TODO : other parameters

    this->score_metric = std::move(
        std::shared_ptr<ClassificationError>(
            new MultiLogLossError(config->n_classes, n_instances)));
}

void ClassificationForest::init() {}

float* ClassificationForest::fitBaseTree(TrainingSet dataset) {
    this->prediction_state = 0;
    this->base_tree = *ID3(dataset, &(Forest::base_tree_config), this->densities.get());

    // Predict with the base tree and compute the gradient of the error
    float* probabilities = classify(
        dataset.data, 
        dataset.n_instances,
        dataset.n_features,
        &(this->base_tree),
        &(Forest::base_tree_config));
    loss_t loss = this->score_metric.get()->computeLoss(probabilities, dataset.targets);
    printf("Iteration %3i / mlog-loss error : %f\n", 0, static_cast<double>(loss));
    return probabilities;
}

void ClassificationForest::fitNewTree(TrainingSet dataset, data_t* gradient) {
    std::shared_ptr<Tree> new_tree = std::shared_ptr<Tree>(ID3(
        { dataset.data, gradient, dataset.n_instances, dataset.n_features },
        &(Forest::grad_trees_config),
        this->densities.get()));
    Forest::trees.push_back(new_tree);
}

data_t* ClassificationForest::predictGradient(std::shared_ptr<Tree> tree, TrainingSet dataset) {
    data_t* predictions = predict(
        dataset.data,
        dataset.n_instances, 
        dataset.n_features,
        tree.get(),
        &(Forest::grad_trees_config));
    return predictions;
}

void ClassificationForest::applySoftmax(float* probabilities, data_t* F_k) {
    size_t n_classes = dynamic_cast<ClassificationError*>(
        this->score_metric.get())->getNumberOfClasses();
    for (uint p = 0; p < Forest::n_instances; p++) {
        data_t softmax_divisor = 0.0;
        for (uint i = 0; i < n_classes; i++) {
            softmax_divisor += std::exp(F_k[p * n_classes + i]);
        }
        for (uint i = 0; i < n_classes; i++) {
            probabilities[p * n_classes + i] = std::exp(
                F_k[p * n_classes + i]) / softmax_divisor;
        }
    }
}

void ClassificationForest::preprocessDensities(TrainingSet dataset) {
    this->densities = std::move(std::shared_ptr<Density>(computeDensities(
        dataset.data, 
        dataset.n_instances, 
        dataset.n_features,
        Forest::base_tree_config.n_classes, 
        Forest::base_tree_config.nan_value, 
        Forest::base_tree_config.partitioning)));
}

void ClassificationForest::fit(TrainingSet dataset) {
    // Compute density functions of all features
    this->preprocessDensities(dataset);

    // Fit the base classification tree
    float* probabilities = this->fitBaseTree(dataset);

    size_t n_classes = Forest::config.n_classes;

    data_t* F_k = static_cast<data_t*>(calloc(
        n_classes * dataset.n_instances, sizeof(data_t)));
    assert(n_classes == this->score_metric.get()->getNumberOfRequiredTrees());
    uint n_boost = 0;
    while (n_boost++ < Forest::config.n_iter) {
        this->score_metric.get()->computeGradient(probabilities, dataset.targets);
        for (uint i = 0; i < n_classes; i++) {
            data_t* gradient = dynamic_cast<MultiLogLossError*>(
                this->score_metric.get())->getGradientAt(i);
            
            // Fit new tree
            this->fitNewTree(dataset, gradient);
            // Predict with new tree
            data_t* predictions = this->predictGradient(Forest::trees.back(), dataset);

            for (uint p = 0; p < dataset.n_instances; p++) {
                // TODO : Compute gamma according to Friedman's formulas
                F_k[p * n_classes + i] -= Forest::config.learning_rate * predictions[p];
            }
            free(predictions);
        }

        this->applySoftmax(probabilities, F_k);

        loss_t loss = this->score_metric.get()->computeLoss(probabilities, dataset.targets);
        printf("Iteration %3i / mlog-loss error : %f\n", n_boost, static_cast<double>(loss));
    }
    free(probabilities);
    free(F_k);
}