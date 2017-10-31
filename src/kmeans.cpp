#include "kmeans.h"

#include "common.h"

#include "../faiss/utils.h"
#include "../faiss/Clustering.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <vector>
#include <algorithm>
#include <iostream>

using namespace std;
using layer_t = IndexHierarchicKmeans::layer_t;


static float fvec_norm_L2(const float *vec, size_t size) {
    return sqrt(faiss::fvec_norm_L2sqr(vec, size));
}

// TODO: Maybe use the shorter extension with square root?
static FloatMatrix normalize_and_expand_vectors(const FloatMatrix& matrix, size_t m) {
    FloatMatrix result;
    result.resize(matrix.vector_count(), matrix.vector_length + m);

    for (size_t i = 0; i < matrix.vector_count(); i++) {
        for (size_t j = 0; j < matrix.vector_length; j++) {
            result.at(i, j) = matrix.at(i, j);
        }
    }

    float max_norm = 0;

    for (size_t i = 0; i < result.vector_count(); i++) {
        float norm = fvec_norm_L2(result.row(i), result.vector_length);
        max_norm = norm > max_norm ? norm : max_norm;
    }

    for (size_t i = 0; i < result.vector_count(); i++) {
        scale(result.row(i), max_norm, result.vector_length);
    }

    for (size_t i = 0; i < result.vector_count(); i++) {
        float norm = fvec_norm_L2(result.row(i), result.vector_length);
        for (size_t j = matrix.vector_length; j < result.vector_length; j++) {
            norm *= norm;
            result.at(i, j) = 0.5 - norm;
        }
    }

    return result;
}

static FloatMatrix expand_queries(const FloatMatrix& matrix, size_t m) {
    FloatMatrix result;
    result.resize(matrix.vector_count(), matrix.vector_length + m);

    for (size_t i = 0; i < matrix.vector_count(); i++) {
        for (size_t j = 0; j < matrix.vector_length; j++) {
            result.at(i, j) = matrix.at(i, j);
        }
        for (size_t j = matrix.vector_length; j < result.vector_length; j++) {
            result.at(i, j) = 0.f;
        }
    }
    return result;
}

static vector<layer_t> make_layers(const FloatMatrix& vectors, size_t L) {
    vector<layer_t> layers = vector<layer_t>(L);

    for (size_t layer_id = 0; layer_id < L; layer_id++) {
        layer_t& layer = layers[layer_id];

        // Compute number of clusters and cluster size on this layer.
        size_t cluster_size = floor(
                pow(vectors.vector_count(), (layer_id + 1) / (float) (L + 1)));

        layer.cluster_num = floor(
                vectors.vector_count() / (float) cluster_size);

        const FloatMatrix& points = (layer_id == 0) ?
               vectors : layers[layer_id - 1].kr.centroids;

        layer.kr = perform_kmeans(points, layer.cluster_num);

        layer.centroid_children.resize(layer.cluster_num);
        for (size_t i = 0; i < layer.kr.assignments.size(); i++) {
            layer.centroid_children[layer.kr.assignments[i]].push_back(i);
        }
    }

    return layers;
}

static vector<size_t> predict(const vector<layer_t>& layers, FloatMatrix& queries, size_t qnum,
        size_t opened_trees, const FloatMatrix& vectors, size_t k_needed = 1) {

    vector<size_t> candidates;
    for (size_t i = 0; i < layers.back().cluster_num; i++) {
        candidates.push_back(i);
    }

    for (size_t layer_id = layers.size() - 1; layer_id != (size_t)(-1); layer_id--) {
        vector<std::pair<float, size_t>> best_centroids;
        for (auto c: candidates) {
            float result = faiss::fvec_inner_product(
                    queries.row(qnum), 
                    layers[layer_id].kr.centroids.row(c),
                    queries.vector_length);

            best_centroids.push_back({result, c});
        }

        if (best_centroids.size() > opened_trees) {
            nth_element(
                    best_centroids.begin(), 
                    best_centroids.begin() + opened_trees,
                    best_centroids.end(),
                    greater<std::pair<float, size_t>>());
            best_centroids.resize(opened_trees);
        }

        candidates.clear();

        for (auto val_cid: best_centroids) {
            size_t cid = val_cid.second;
            candidates.insert(candidates.end(), 
                    layers[layer_id].centroid_children[cid].begin(),
                    layers[layer_id].centroid_children[cid].end()
            );
        }
    }
    // Last layer - find best match.
    vector<std::pair<float, size_t>> best_points;
    for (auto c: candidates) {
        float result = faiss::fvec_inner_product(
                queries.row(qnum), 
                vectors.row(c),
                queries.vector_length);

        best_points.push_back({result, c});
    }
    if (best_points.size() > k_needed) {
        nth_element(
                best_points.begin(), 
                best_points.begin() + k_needed,
                best_points.end(),
                greater<std::pair<float, size_t>>());
        best_points.resize(k_needed);
    }
    sort(best_points.rbegin(), best_points.rend());

    vector<size_t> res;
    for (size_t i = 0; i < best_points.size(); i++) {
        res.push_back(best_points[i].second);
    }
    return res;
}

IndexHierarchicKmeans::IndexHierarchicKmeans(
        size_t dim, size_t m, size_t layers_count, size_t opened_trees):
    Index(dim, faiss::METRIC_INNER_PRODUCT),
    layers_count(layers_count), m(m), opened_trees(opened_trees) {}

void IndexHierarchicKmeans::add(idx_t n, const float* data) {
    vectors_original.resize(n, d);
    memcpy(vectors_original.data.data(), data, n * d * sizeof(float));
    vectors = normalize_and_expand_vectors(vectors_original, m);
    layers = make_layers(vectors, layers_count);
}

void IndexHierarchicKmeans::reset() {
    vectors.data.clear();
    vectors_original.data.clear();
    layers.clear();
}

void IndexHierarchicKmeans::search(idx_t n, const float* data, idx_t k, 
        float* distances, idx_t* labels) const {
    // TODO: ugly copying tbh - should use given array
    FloatMatrix queries_original;
    queries_original.resize(n, d);
    memcpy(queries_original.data.data(), data, n * d * sizeof(float));
    FloatMatrix queries = expand_queries(queries_original, m);

    FlatMatrix<idx_t> labels_matrix;
    labels_matrix.resize(n, k);
    for (size_t i = 0; i < queries.vector_count(); i++) {
        vector<size_t> predictions = predict(layers, queries, i, opened_trees, vectors, k);
        for (idx_t j = 0; j < k; j++) {
            labels_matrix.at(i, j) = (size_t(j) < predictions.size()) ? predictions[j] : -1;
        }

        for (idx_t j = 0; j < k; j++) {
            idx_t lab = labels_matrix.at(i, j);
            if (lab != -1) {
                distances[i * k + j] = faiss::fvec_inner_product(
                    vectors_original.row(lab),
                    queries_original.row(i),
                    d
                );
            }
        }
    }
    memcpy(labels, labels_matrix.data.data(), n * k * sizeof(idx_t));
}


void main_kmeans() {
    const char* input_file;
    const char* query_file;
    size_t m, L;
    size_t opened_trees = 2;

    input_file = "input";
    query_file = "queries";
    m = 3;
    L = 2;

    auto vectors_original = load_text_file<float>(input_file);
    auto vectors = normalize_and_expand_vectors(vectors_original, m);

    vector<layer_t> layers = make_layers(vectors, L);

    auto queries_original = load_text_file<float>(query_file);
    auto queries = expand_queries(queries_original, m);

    assert(vectors.vector_length == queries.vector_length and
               "Queries and Vectors dimension mismatch!");

    vector<int> predictions;
    for (size_t i = 0; i < queries.vector_count(); i++) {
        vector<size_t> res = predict(layers, queries, i, opened_trees, vectors);
        predictions.push_back(res[0]);
        std::cout << "Query " << i << ": " << res[0] << std::endl;
    }
}
