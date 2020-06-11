#ifndef HYPERGRAPH_H
#define HYPERGRAPH_H

#include <vector>
#include <unordered_map>
#include <iostream>

namespace HyperPart {

struct HypergraphEdge {
	std::vector<int> nodes;
	int weight = 1;
};

struct HypergraphNode {
	std::vector<int> edges;
	int partition = -1;
	int area = 1;
	bool fixed = false;
};

struct Hypergraph {
	std::vector<HypergraphEdge> edges;
	std::vector<HypergraphNode> nodes;
	void dump(std::ostream &out) const;
	void read(std::istream &in);
};

struct PartitionConstraint {
	int min_nodes, max_nodes;
};

void partition_hypergraph(Hypergraph &g, const std::vector<PartitionConstraint> &partitions);

};

#endif
