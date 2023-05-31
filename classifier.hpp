#ifndef CLASSIFIER_H
#define CLASSIFIER_H

#include <vector>
#include <random>
#include <cmath>

#include "features.hpp"
#include "labels.hpp"
#include "constants.hpp"
#include "point_io.hpp"
#include "statistics.hpp"

enum Regularization { None, LocalSmooth, GraphCut };
Regularization parseRegularization(const std::string &regularization);

enum ClassifierType { RandomForest, GradientBoostedTrees };
ClassifierType fingerprint(const std::string &modelFile);


template <typename F, typename I>
void getTrainingData(const std::vector<std::string> &filenames,
    double *startResolution,
    const int numScales,
    const double radius,
    const int maxSamples,
    const std::vector<int> &asprsClasses,
    F storeFeatures,
    I init) {
    auto labels = getTrainingLabels();

    bool trainSubset = asprsClasses.size() > 0;
    std::array<bool, 255> trainClass;

    if (trainSubset) {
        trainClass.fill(false);

        auto asprsToTrain = getAsprs2TrainCodes();
        for (auto &c : asprsClasses) {
            trainClass[asprsToTrain[c]] = true;
        }
    }

    for (size_t i = 0; i < filenames.size(); i++) {
        std::cout << "Processing " << filenames[i] << std::endl;
        auto pointSet = readPointSet(filenames[i]);
        if (!pointSet->hasLabels()) {
            std::cout << filenames[i] << " has no labels, skipping..." << std::endl;
            continue;
        }

        if (*startResolution == -1.0) {
            *startResolution = pointSet->spacing(); // meters
            std::cout << "Starting resolution: " << *startResolution << std::endl;
        }

        auto scales = computeScales(numScales, pointSet, *startResolution, radius);
        auto features = getFeatures(scales);
        std::cout << "Features: " << features.size() << std::endl;

        if (i == 0) init(features.size(), labels.size());

        std::vector<std::size_t> count(labels.size(), 0);
        std::vector<bool> sampled(pointSet->count(), false);
        std::vector<std::pair<size_t, int> > idxes;

        for (size_t i = 0; i < pointSet->count(); i++) {
            int g = pointSet->labels[i];
            if (g != LABEL_UNASSIGNED) {
                if (trainSubset && !trainClass[g]) continue;

                size_t idx = pointSet->pointMap[i];
                if (!sampled[idx]) {
                    idxes.push_back(std::make_pair(idx, g));
                    count[std::size_t(g)]++;
                    sampled[idx] = true;
                }
            }
        }

        size_t samplesPerLabel = std::numeric_limits<size_t>::max();
        for (std::size_t i = 0; i < labels.size(); i++) {
            if (count[i] > 0) samplesPerLabel = std::min(count[i], samplesPerLabel);
        }
        samplesPerLabel = std::min<size_t>(samplesPerLabel, maxSamples);
        std::vector<std::size_t> added(labels.size(), 0);

        std::cout << "Samples per label: " << samplesPerLabel << std::endl;

        std::random_device rd;
        std::mt19937 ranGen(rd());
        std::shuffle(idxes.begin(), idxes.end(), ranGen);

        for (const auto &p : idxes) {
            size_t idx = p.first;
            int g = p.second;
            if (added[std::size_t(g)] < samplesPerLabel) {
                storeFeatures(features, idx, g);
                added[std::size_t(g)]++;
            }
        }

        for (std::size_t i = 0; i < labels.size(); i++)
            std::cout << " * " << labels[i].getName() << ": " << added[i] << " / " << count[i] << std::endl;

        // Free up memory for next
        for (size_t i = 0; i < scales.size(); i++) delete scales[i];
        for (size_t i = 0; i < features.size(); i++) delete features[i];
        RELEASE_POINTSET(pointSet);
    }
}

void alphaExpansionGraphcut(
    const std::vector<std::pair<std::size_t, std::size_t>> &inputGraph,
    const std::vector<float> &edgeCostMap,
    const std::vector<std::vector<double>> &vertexLabelCostMap,
    const std::vector<std::size_t> &vertexLabelMap);

template <typename T, typename F>
void classifyData(PointSet &pointSet,
    F evaluateFunc,
    const std::vector<Feature *> &features,
    const std::vector<Label> &labels,
    const Regularization regularization,
    const double regRadius,
    const bool useColors,
    const bool unclassifiedOnly,
    const bool evaluate,
    const std::vector<int> &skip,
    const std::string &statsFile) {

    std::cout << "Classifying..." << std::endl;
    pointSet.base->labels.resize(pointSet.base->count());

    if (regularization == None) {
        #pragma omp parallel
        {
            std::vector<T> probs(labels.size(), 0.);
            std::vector<T> ft(features.size());

            #pragma omp for
            for (long long int i = 0; i < pointSet.base->count(); i++) {
                for (std::size_t f = 0; f < features.size(); f++) {
                    ft[f] = features[f]->getValue(i);
                }

                evaluateFunc(ft.data(), probs.data());

                // Find highest probability
                int bestClass = 0;
                T bestClassVal = 0.;

                for (std::size_t j = 0; j < probs.size(); j++) {
                    if (probs[j] > bestClassVal) {
                        bestClass = j;
                        bestClassVal = probs[j];
                    }
                }

                pointSet.base->labels[i] = bestClass;
            }
        } // end pragma omp

    }
    else if (regularization == LocalSmooth) {
        std::vector<std::vector<T> > values(labels.size(), std::vector<T>(pointSet.base->count(), -1.));

        #pragma omp parallel
        {

            std::vector<T> probs(labels.size(), 0.);
            std::vector<T> ft(features.size());

            #pragma omp for
            for (long long int i = 0; i < pointSet.base->count(); i++) {
                for (std::size_t f = 0; f < features.size(); f++) {
                    ft[f] = features[f]->getValue(i);
                }

                evaluateFunc(ft.data(), probs.data());

                for (std::size_t j = 0; j < labels.size(); j++) {
                    values[j][i] = probs[j];
                }
            }

        }

        std::cout << "Local smoothing..." << std::endl;

        #pragma omp parallel
        {

            std::vector<nanoflann::ResultItem<size_t, float>> radiusMatches;
            std::vector<T> mean(values.size(), 0.);
            const auto index = pointSet.base->getIndex<KdTree>();

            #pragma omp for schedule(dynamic, 1)
            for (long long int i = 0; i < pointSet.base->count(); i++) {
                size_t numMatches = index->radiusSearch(&pointSet.base->points[i][0], regRadius, radiusMatches);
                std::fill(mean.begin(), mean.end(), 0.);

                for (size_t n = 0; n < numMatches; n++) {
                    for (std::size_t j = 0; j < values.size(); ++j) {
                        mean[j] += values[j][radiusMatches[n].first];
                    }
                }

                int bestClass = 0;
                T bestClassVal = 0.f;
                for (std::size_t j = 0; j < mean.size(); j++) {
                    mean[j] /= numMatches;
                    if (mean[j] > bestClassVal) {
                        bestClassVal = mean[j];
                        bestClass = j;
                    }
                }

                pointSet.base->labels[i] = bestClass;
            }

        }
    }
    else if (regularization == GraphCut)
    {

        std::cout << "Using graph cut..." << std::endl;

        // Calculate bounding boxes

        constexpr int minSubdivisions = 4;
        constexpr float strength = 0.2f;
        constexpr int neighbors = 12;
        const auto bbox = pointSet.getBbox();

        float Dx = bbox.xmax() - bbox.xmin();
        float Dy = bbox.ymax() - bbox.ymin();
        float A = Dx * Dy;
        float a = A / minSubdivisions;
        float l = std::sqrt(a);
        std::size_t nbX = static_cast<std::size_t>(Dx / l) + 1;
        std::size_t nbY = static_cast<std::size_t>(A / nbX / a) + 1;
        std::size_t nb = nbX * nbY;

        std::vector<Bbox3> bboxes;
        bboxes.reserve(nb);

        for (std::size_t x = 0; x < nbX; ++x)
            for (std::size_t y = 0; y < nbY; ++y)
            {
                bboxes.push_back
                (Bbox3(bbox.xmin() + Dx * (x / static_cast<float>(nbX)),
                    bbox.ymin() + Dy * (y / static_cast<float>(nbY)),
                    bbox.zmin(),
                    (x == nbX - 1 ? bbox.xmax() : bbox.xmin() + Dx * ((x + 1) / static_cast<float>(nbX))),
                    (y == nbY - 1 ? bbox.ymax() : bbox.ymin() + Dy * ((y + 1) / static_cast<float>(nbY))),
                    bbox.zmax()));
            }

        std::cerr << "Using" << nbX * nbY << " divisions with size " << Dx / nbX << " " << Dy / nbY << std::endl;

        // Assign points to bounding boxes

        std::vector<std::vector<std::size_t> > indices(nb);
        std::vector<std::pair<std::size_t, std::size_t> > inputToIndices(pointSet.base->count());

        for (std::size_t i = 0; i < pointSet.base->count(); ++i)
        {
            const auto &p = pointSet.base->points[i];
            std::size_t idx = 0;
            for (std::size_t j = 0; j < bboxes.size(); ++j)
            {
                if (bboxes[j].contains(p))
                {
                    idx = j;
                    break;
                }
            }
            inputToIndices[i] = std::make_pair(i, idx);
            indices[idx].push_back(i);
        }

        std::cerr << "Assigning points to bounding boxes done" << std::endl;


        //#pragma omp parallel for
        for (std::size_t sub = 0; sub < indices.size(); ++sub)
        {
            if (indices[sub].empty())
                continue;

            std::vector<std::pair<std::size_t, std::size_t> > edges;
            std::vector<float> edgeWeights;
            std::vector probabilityMatrix(labels.size(), std::vector(indices[sub].size(), 0.));
            std::vector<std::size_t> assignedLabel(indices[sub].size());

            const auto index = pointSet.base->getIndex<KdTree>();

            for (std::size_t j = 0; j < indices[sub].size(); ++j)
            {
                std::size_t s = indices[sub][j];

                std::size_t nIndices[12];
                float nDistances[12];

                size_t numMatches = index->knnSearch(&pointSet.base->points[s][0], neighbors, nIndices, nDistances);

                for (std::size_t i = 0; i < numMatches; ++i) {

                    const auto neighbor = nIndices[i];

                    if (sub == inputToIndices[neighbor].first
                        && j != inputToIndices[neighbor].second)
                    {
                        edges.push_back(std::make_pair(j, inputToIndices[neighbor].second));
                        edgeWeights.push_back(strength);
                    }

                }

                std::vector<T> values(labels.size(), 0.);
                std::vector<T> ft(features.size());

                for (std::size_t f = 0; f < features.size(); f++) {
                    ft[f] = features[f]->getValue(s);
                }

                evaluateFunc(ft.data(), values.data());
                std::size_t nbClassBest = 0;
                float valClassBest = 0.f;
                for (std::size_t k = 0; k < labels.size(); ++k)
                {
                    float value = values[k];
                    probabilityMatrix[k][j] = -std::log(value);

                    if (valClassBest < value)
                    {
                        valClassBest = value;
                        nbClassBest = k;
                    }
                }
                assignedLabel[j] = nbClassBest;
            }

            alphaExpansionGraphcut(edges, edgeWeights, probabilityMatrix, assignedLabel);
            for (std::size_t i = 0; i < assignedLabel.size(); ++i)
                pointSet.base->labels[indices[sub][i]] = assignedLabel[i];

        }


    }
    else {
        throw std::runtime_error("Invalid regularization");
    }

    if (!useColors && !pointSet.hasLabels()) pointSet.labels.resize(pointSet.count());
    std::vector<bool> skipMap(255, false);
    for (size_t i = 0; i < skip.size(); i++) {
        const int skipClass = skip[i];
        if (skipClass >= 0 && skipClass <= 255) skipMap[skipClass] = true;
    }

    auto train2asprsCodes = getTrain2AsprsCodes();

    Statistics stats(labels);

    #pragma omp parallel for
    for (long long int i = 0; i < pointSet.count(); i++) {
        const size_t idx = pointSet.pointMap[i];

        const int bestClass = pointSet.base->labels[idx];
        auto label = labels[bestClass];

        if (evaluate) {
            stats.record(bestClass, pointSet.labels[i]);
        }

        bool update = true;
        const bool hasLabels = pointSet.hasLabels();

        // if unclassifiedOnly, do not update points with an existing classification
        if (unclassifiedOnly && hasLabels
            && pointSet.labels[i] != LABEL_UNCLASSIFIED) update = false;

        const int asprsCode = label.getAsprsCode();
        if (skipMap[asprsCode]) update = false;

        if (update) {
            if (useColors) {
                auto color = label.getColor();
                pointSet.colors[i][0] = color.r;
                pointSet.colors[i][1] = color.g;
                pointSet.colors[i][2] = color.b;
            }
            else {
                pointSet.labels[i] = asprsCode;
            }
        }
        else if (hasLabels) {
            // We revert training codes back to ASPRS
            pointSet.labels[i] = train2asprsCodes[pointSet.labels[i]];
        }
    }

    if (evaluate) {
        stats.finalize();
        stats.print();
        if (!statsFile.empty()) stats.writeToFile(statsFile);
    }
}

#endif

