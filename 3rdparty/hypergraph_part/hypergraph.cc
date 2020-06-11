#include "hypergraph.h"
#include "hypergraph_util.h"

#include <sstream>
#include <algorithm>

namespace HyperPart {
	void Hypergraph::dump(std::ostream &out) const {
		out << GetSize(nodes) << " " << GetSize(edges) << std::endl;
		for (const auto &n : nodes) {
			out << "N " << n.partition << " " << n.fixed << std::endl;
		}
		for (const auto &e : edges) {
			out << "E " << e.weight;
			for (int n : e.nodes)
				out << " " << n;
			out << std::endl;
		}
	}

	void Hypergraph::read(std::istream &in) {
		nodes.clear();
		edges.clear();
		int nedges = 0 , nnodes = 0;
		in >> nnodes >> nedges;
		nodes.resize(nnodes);
		edges.resize(nedges);
		std::string line;
		for (int i = 0; i < nnodes; i++) {
			do {
				HYPER_ASSERT(in);
				std::getline(in, line);
			} while (std::all_of(line.begin(), line.end(), [] (char c) { return std::isblank(c); } ));
			std::stringstream lread(line);
			char c;
			int p;
			bool f;
			lread >> c >> p >> f;
			HYPER_ASSERT(c == 'N');
			nodes.at(i).partition = p;
			nodes.at(i).fixed = f;
		}
		for (int i = 0; i < nedges; i++) {
			do {
				HYPER_ASSERT(in);
				std::getline(in, line);
			} while (std::all_of(line.begin(), line.end(), [] (char c) { return std::isblank(c); } ));
			std::stringstream lread(line);
			char c;
			int w, n;
			lread >> c >> w;
			HYPER_ASSERT(c == 'E');
			edges.at(i).weight = w;
			while (lread >> n) {
				nodes.at(n).edges.push_back(i);
				edges.at(i).nodes.push_back(n);
			}
		}
	}
}

