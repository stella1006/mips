#include "../src/common.h"
#include "../src/bench.h"

#include "faiss/AutoTune.h"


faiss::Index* get_trained_index(const FloatMatrix& xt) {
    faiss::Index* index = faiss::index_factory(xt.vector_length, "IVF4096,Flat");
    index->train(xt.vector_count(), xt.data.data());
    return index;
}

int main() {
    bench(get_trained_index);
}