#include "lsh.h"

bool debug_messages=false;

void
insert_into_clusters(const SqlDatabase::TransactionPtr &tx, int cluster, int function_id, int index_within_function,
                     int vectors_row, double dist)
{
    int id = tx->statement("select coalesce(max(id),0)+1 from clusters")->execute_int();

    tx->statement("insert into clusters"
                  // 0   1        2            3                      4            5
                  " (id, cluster, function_id, index_within_function, vectors_row, dist)"
                  " values (?, ?,?,?,?,?)")
        ->bind(0, id)
        ->bind(1, cluster)
        ->bind(2, function_id)
        ->bind(3, index_within_function)
        ->bind(4, vectors_row)
        ->bind(5, dist)
        ->execute();
}

void
insert_into_postprocessed_clusters(const SqlDatabase::TransactionPtr &tx, int cluster, int function_id, int index_within_function,
                                   int vectors_row, double dist)
{
    tx->statement("insert into postprocessed_clusters"
                  // 0        1            2                      3            4
                  " (cluster, function_id, index_within_function, vectors_row, dist)"
                  " values(?,?,?,?,?)")
        ->bind(0, cluster)
        ->bind(1, function_id)
        ->bind(2, index_within_function)
        ->bind(3, vectors_row)
        ->bind(4, dist)
        ->execute();
}

void
get_run_parameters(const SqlDatabase::TransactionPtr &tx, int& windowSize, int& stride)
{
    windowSize = tx->statement("select window_size from run_parameters limit 1")->execute_int();
    stride = tx->statement("select stride from run_parameters limit 1")->execute_int();
    assert (windowSize != 0);
    assert (stride != 0);
}

void
compute_vector_range(const scoped_array_with_size<VectorEntry>& vectors, size_t numVectorElements,
                     scoped_array_with_size<uint16_t>& upperBounds, size_t& totalRange)
{
    memset(&upperBounds[0], 0, sizeof(uint16_t) * numVectorElements);
    for (size_t i = 0; i < vectors.size(); ++i) {
        const VectorEntry& ve = vectors[i];
        if (ve.compressedCounts.size() == 0)
            continue; // Empty entry
        elementwiseMax(ve.compressedCounts.get(), ve.compressedCounts.size(), upperBounds.get());
    }

    totalRange = 0;
    for (size_t i = 0; i < numVectorElements; ++i)
        totalRange += upperBounds[i];
    if (debug_messages == true)
        std::cout << "Total range is " << totalRange << std::endl;
}

HammingHashFunctionSet::HammingHashFunctionSet(const scoped_array_with_size<VectorEntry>& vectors, size_t k, size_t l,
                                               double /* r */, size_t numVectorElements, size_t hashTableNumBuckets)
    : k(k), l(l), numVectorElements(numVectorElements), hashTableNumBuckets(hashTableNumBuckets), upperBounds(numVectorElements),
      totalRange(totalRange), hashFunctionVectorIndexes(l * k), hashFunctionVectorCompareValues(l * k),
      hashFunctionCoeffs(l * k) {
    compute_vector_range(vectors, numVectorElements, upperBounds, totalRange);
    create_hash_functions();
}

void
HammingHashFunctionSet::create_hash_functions()
{
    boost::mt19937 rng;
    boost::uniform_int<> hammingDimUniform(0, totalRange - 1);
    boost::variate_generator<boost::mt19937&, boost::uniform_int<> > hammingDimGenerator(rng, hammingDimUniform);

    for (size_t i = 0; i < l; ++i) { //Iterate over hash tables
        scoped_array_with_size<size_t> hashFunctionIndexes(k);
        for (size_t j = 0; j < k; ++j) { // Iterate over selected indexes in the bit vector
            size_t rawIndex = hammingDimGenerator();
            hashFunctionIndexes[j] = rawIndex;
        }
        std::sort(&hashFunctionIndexes[0], &hashFunctionIndexes[k]); // For faster lookups
        for (size_t j = 0; j < k; ++j) { //Iterate over selected indexes in the bit vector
            //Converting the into indexes in the original vector
            size_t rawIndex = hashFunctionIndexes[j];
            size_t vi, vc;
            for (vi = 0; vi < numVectorElements; ++vi) {
                if (rawIndex < upperBounds[vi]) {
                    vc = rawIndex;
                    break;
                } else {
                    rawIndex -= upperBounds[vi];
                }
            }
            if (vi >= numVectorElements) {
                std::cerr << "Bad index computation" << std::endl;
                abort();
            }
            if (vc >= upperBounds[vi]) {
                std::cerr << "Overflow" << std::endl;
                abort();
            }
            hashFunctionVectorIndexes[i * k + j] = vi;
            hashFunctionVectorCompareValues[i * k + j] = vc;
        }
    }
    boost::uniform_int<> hashBucketUniform(1, hashTableNumBuckets - 1);
    boost::variate_generator<boost::mt19937&, boost::uniform_int<> > hashBucketGenerator(rng, hashBucketUniform);
    for (size_t i = 0; i < l; ++i) {
        for (size_t j = 0; j < k; ++j)
            hashFunctionCoeffs[i * k + j] = hashBucketGenerator();
    }

    if (debug_messages == true)
        std::cerr << "Have " << l << " hash function(s) with " << k << " component(s) each" << std::endl;
}

void
HammingHashFunctionSet::compute_hashes(const scoped_array_with_size<uint8_t>& compressedCounts,
                                       const boost::scoped_array<size_t>& hashes) const
{
    for (size_t functionIndex = 0; functionIndex < l; ++functionIndex) {
        hashes[functionIndex] = computeL1Hash(compressedCounts.get(), compressedCounts.size(), k,
                                              &hashFunctionVectorIndexes[functionIndex * k],
                                              &hashFunctionVectorCompareValues[functionIndex * k],
                                              &hashFunctionCoeffs[functionIndex * k], hashTableNumBuckets);
    }
}

StableDistributionHashFunctionSet::StableDistributionHashFunctionSet(const scoped_array_with_size<VectorEntry>& /* vectors */,
                                                                     size_t k, size_t l, double r, size_t numVectorElements,
                                                                     size_t hashTableNumBuckets)
    : r(r), k(k), l(l), numVectorElements(numVectorElements), hashTableNumBuckets(hashTableNumBuckets),
      hashFunctionMatrix(l * k * numVectorElements), hashFunctionBiases(l * k), hashFunctionCoeffs(l * k) {
    create_hash_functions();
}

void
StableDistributionHashFunctionSet::create_hash_functions()
{
    boost::mt19937 rng;
    boost::variate_generator<boost::mt19937&, boost::normal_distribution<> > eltGenerator(rng, boost::normal_distribution<>());
    boost::variate_generator<boost::mt19937&, boost::uniform_real<> > biasGenerator(rng, boost::uniform_real<>(0, r));

    for (size_t i = 0; i < l; ++i) { //Iterate over hash tables
        for (size_t j = 0; j < k; ++j) { // Iterate over elements in a combined hash function
            hashFunctionBiases[i * k + j] = (float)biasGenerator();
            for (size_t x = 0; x < numVectorElements; ++x) // Iterate over length of vectors
                hashFunctionMatrix[i * k * numVectorElements + j * numVectorElements + x] = (float)eltGenerator();
        }
    }
    boost::uniform_int<> hashBucketUniform(1, hashTableNumBuckets - 1);
    boost::variate_generator<boost::mt19937&, boost::uniform_int<> > hashBucketGenerator(rng, hashBucketUniform);
    for (size_t i = 0; i < l; ++i) { //Iterate over hash tables
        for (size_t j = 0; i < k; ++i)
            hashFunctionCoeffs[i * k + j] = hashBucketGenerator();
    }

    if (debug_messages == true)
        std::cerr << "Have " << l << " hash function(s) with " << k << " component(s) each" << std::endl;
}

void
StableDistributionHashFunctionSet::compute_hashes(const scoped_array_with_size<uint8_t>& compressedCounts,
                                                  const boost::scoped_array<size_t>& hashes) const
{
    size_t hv = 0;
    boost::scoped_array<double> dps(new double[l * k]);
    multipleDotProducts(compressedCounts.get(), compressedCounts.size(), &hashFunctionMatrix[0], l * k, numVectorElements,
                        &dps[0]);
    for (size_t functionIndex = 0; functionIndex < l; ++functionIndex) {
        for (size_t i = 0; i < k; ++i) {
            int64_t val = (int64_t)floor((dps[functionIndex * k + i] + hashFunctionBiases[functionIndex * k + i]) / r);
            size_t valMod = 0;
            if (val < 0) {
                valMod = (hashTableNumBuckets - ((size_t)(-val) % hashTableNumBuckets)) % hashTableNumBuckets;
            } else {
                valMod = (size_t)val % hashTableNumBuckets;
            }
            hv = (hv + (uint64_t)val * hashFunctionCoeffs[functionIndex * k + i]) % hashTableNumBuckets;
        }
        hashes[functionIndex] = hv;
    }
}

double
L1DistanceObject::operator()(const scoped_array_with_size<uint8_t>& a, const boost::scoped_array<uint16_t>& b) const
{
    return l1distance(a.get(), a.size(), b.get());
}

double
L2DistanceObject::operator()(const scoped_array_with_size<uint8_t>& a, const boost::scoped_array<uint16_t>& b) const
{
    return sqrt(l2distanceSquared(a.get(), a.size(), b.get()));
}
