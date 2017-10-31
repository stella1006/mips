#include "../src/common.h"
#include "../src/bench.h"
#include "../src/kmeans.h"

size_t m;
size_t layers_count;
size_t opened_trees;

faiss::Index* get_trained_index(const FloatMatrix& xt) {
    faiss::Index* index = new IndexHierarchicKmeans(xt.vector_length, m, layers_count, opened_trees);
    index->train(xt.vector_count(), xt.data.data());
    return index;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Arguments missing, terminating.\n");
    } else {
        m = atoi(argv[1]);
        layers_count = atoi(argv[2]);
        opened_trees = atoi(argv[3]);

        faiss::Index* index = bench_train(get_trained_index);
        bench_add(index);

        for (int i = 3; i < argc; i++) {
            printf("Querying using opened_trees = %d\n", atoi(argv[i]));
            ((IndexHierarchicKmeans*) index)->opened_trees = atoi(argv[i]);
            bench_query(index);
        }
    }
}
