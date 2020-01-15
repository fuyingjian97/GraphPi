#include "../include/graph.h"
#include "../include/vertex_set.h"
#include "../include/common.h"
#include <cstdio>
#include <sys/time.h>
#include <unistd.h>
#include <cstdlib>
#include <omp.h>
#include <algorithm>
#include <cstring>
#include <mpi.h>
#include <atomic>
#include <queue>
#include <iostream>

int Graph::intersection_size(int v1,int v2) {
    int l1, r1;
    get_edge_index(v1, l1, r1);
    int l2, r2;
    get_edge_index(v2, l2, r2);
    int ans = 0;
    while(l1 < r1 && l2 < r2) {
        if(edge[l1] < edge[l2]) {
            ++l1;
        }
        else {
            if(edge[l2] < edge[l1]) {
                ++l2;
            }
            else {
                ++l1;
                ++l2;
                ++ans;
            }
        }
    }
    return ans;
}

int Graph::intersection_size_clique(int v1,int v2) {
    int l1, r1;
    get_edge_index(v1, l1, r1);
    int l2, r2;
    get_edge_index(v2, l2, r2);
    int min_vertex = v2;
    int ans = 0;
    if (edge[l1] >= min_vertex || edge[l2] >= min_vertex)
        return 0;
    while(l1 < r1 && l2 < r2) {
        if(edge[l1] < edge[l2]) {
            if (edge[++l1] >= min_vertex)
                break;
        }
        else {
            if(edge[l2] < edge[l1]) {
                if (edge[++l2] >= min_vertex)
                    break;
            }
            else {
                ++ans;
                if (edge[++l1] >= min_vertex)
                    break;
                if (edge[++l2] >= min_vertex)
                    break;
            }
        }
    }
    return ans;
}

long long Graph::triangle_counting() {
    long long ans = 0;
    for(int v = 0; v < v_cnt; ++v) {
        // for v in G
        int l, r;
        get_edge_index(v, l, r);
        for(int v1 = l; v1 < r; ++v1) {
            //for v1 in N(v)
            ans += intersection_size(v,edge[v1]);
        }
    }
    ans /= 6;
    return ans;
}

long long Graph::triangle_counting_mt(int thread_count) {
    long long ans = 0;
#pragma omp parallel num_threads(thread_count)
    {
        tc_mt(&ans);
    }
    return ans;
}

void Graph::tc_mt(long long *global_ans) {
    long long my_ans = 0;
    #pragma omp for schedule(dynamic)
    for(int v = 0; v < v_cnt; ++v) {
        // for v in G
        int l, r;
        get_edge_index(v, l, r);
        for(int v1 = l; v1 < r; ++v1) {
            if (v <= edge[v1])
                break;
            //for v1 in N(v)
            my_ans += intersection_size_clique(v,edge[v1]);
        }
    }
    #pragma omp critical
    {
        *global_ans += my_ans;
    }
}

long long Graph::triangle_counting_mpi(int thread_count) {
	long long node_ans = 0, tot_ans = 0;
	int comm_sz, my_rank, idlethreadcnt = 1, mynodel, mynoder, blocksize, globalv;
	std::atomic_flag lock[thread_count];
	std::queue<int> q;
	const int MAXN = 1 << 22;
	static int data[24][MAXN], qrynode[64], qrydest[64];
#pragma omp parallel num_threads(thread_count)
    {
#pragma omp master
		{
			int provided;
			MPI_Init_thread(NULL, NULL, MPI_THREAD_FUNNELED, &provided);
			MPI_Comm_size(MPI_COMM_WORLD, &comm_sz);
			MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
			blocksize = (v_cnt + comm_sz - 1) / comm_sz;
			mynodel = blocksize * my_rank;
			mynoder = my_rank < comm_sz - 1 ? blocksize * (my_rank + 1) : v_cnt;
			globalv = mynodel;
		}
#pragma omp barrier //mynodel have to be calculated before running other threads
#pragma omp master
		{
			const int REQ = 0, ANS = 1, IDLE = 2, END = 3;
			static int recv[MAXN], send[MAXN];
			MPI_Request sendrqst, recvrqst;
			MPI_Status status;
			MPI_Irecv(recv, sizeof(recv), MPI_INT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &recvrqst);
			int idlenodecnt = 0;
			bool waitforans = false;
			for (;;) {
				int testflag = 0;
				MPI_Test(&recvrqst, &testflag, &status);
				if (testflag) {
					int m;
					MPI_Get_count(&status, MPI_INT, &m);
					if (recv[0] == REQ) {
						int l, r;
						get_edge_index(recv[1], l, r);
						send[0] = ANS;
						memcpy(send + 1, edge + l, sizeof(edge[0]) * (r - l));
						MPI_Isend(send, r - l + 1, MPI_INT, status.MPI_SOURCE, 0, MPI_COMM_WORLD, &sendrqst);
					}
					else if (recv[0] == ANS) {
						int node;
#pragma omp critical
						{
							node = q.front();
							q.pop();
						}
						memcpy(data[node], recv + 1, sizeof(recv[0]) * (m - 1));
						data[node][m - 1] = -1;
						lock[node].clear();
						waitforans = false;
					}
					else if (recv[0] == IDLE) {
						idlenodecnt++;
						tot_ans += ((recv[1] << 30) | recv[2]);
					}
					else if (recv[0] == END) {
						tot_ans = ((recv[1] << 30) | recv[2]);
						break;
					}
					MPI_Irecv(recv, sizeof(recv), MPI_INT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &recvrqst);
				}
				if (!waitforans && !q.empty()) {
					send[0] = REQ;
					int node;
#pragma omp critical
					{
						node = q.front();
					}
					send[1] = qrynode[node];
					MPI_Isend(send, 2, MPI_INT, qrydest[node], 0, MPI_COMM_WORLD, &sendrqst);
					waitforans = true;
				}
				if (idlethreadcnt == thread_count) {
					idlethreadcnt = 0;
					if (my_rank) {
						send[0] = IDLE;
						send[1] = node_ans >> 30;
						send[2] = node_ans & ((1ll << 30) - 1);
						MPI_Isend(send, 3, MPI_INT, 0, 0, MPI_COMM_WORLD, &sendrqst);
					}
					else {
						idlenodecnt++;
#pragma omp atomic
						tot_ans += node_ans;
					}
				}
				if (idlenodecnt == comm_sz) {
					for (int i = 1; i < comm_sz; i++) {
						send[0] = END;
						send[1] = tot_ans >> 30;
						send[2] = tot_ans & ((1ll << 30) - 1);
						MPI_Isend(send, 3, MPI_INT, i, 0, MPI_COMM_WORLD, &sendrqst);
					}
					break;
				}
			}
			MPI_Finalize();
		}
		long long thread_ans = 0;
		int my_thread_num = omp_get_thread_num();
		lock[my_thread_num].test_and_set();
/*#pragma omp for schedule(dynamic)
		for(int v = 0; v < v_cnt; ++v) {*/
		for (int v;;) {
			bool breakflag = false;
#pragma omp critical
			{
				if (globalv == mynoder) breakflag = true;
				else {
					v = globalv;
					globalv++;
				}
			}
			if (breakflag) break;
			if (v % 100 == 0) {
				printf("%d\n", v);
				fflush(stdout);
			}
			// for v in G
			int l, r;
			get_edge_index(v, l, r);
			for(int v1 = l; v1 < r; ++v1) {
				//for v1 in N(v)
				int v2 = edge[v1];
				if (mynodel <= v2 && v2 < mynoder) {
					thread_ans += intersection_size(v, v2);
				}
				else {
					int l1, r1;
					get_edge_index(v, l1, r1);
					qrynode[my_thread_num] = v2;
					qrydest[my_thread_num] = v2 / blocksize;
#pragma omp critical
					{
						q.push(my_thread_num);
					}
					for (;lock[my_thread_num].test_and_set(););
					for (int l2 = 0; l1 < r1 && ~data[my_thread_num][l2];) {
						if(edge[l1] < data[my_thread_num][l2]) {
							++l1;
						}
						else if(edge[l1] > data[my_thread_num][l2]) {
							++l2;
						}
						else {
							++l1;
							++l2;
							++thread_ans;
						}
					}
				}
			}
		}
#pragma omp critical
		{
			node_ans += thread_ans;
			idlethreadcnt++;
		}
	}
	return tot_ans / 6ll;
}

void Graph::get_edge_index(int v, int& l, int& r) const
{
	l = vertex[v];
	r = vertex[v + 1];
}

void Graph::pattern_matching_func(const Schedule& schedule, VertexSet* vertex_set, VertexSet& subtraction_set, long long& local_ans, int depth, bool clique)
{
	int loop_set_prefix_id = schedule.get_loop_set_prefix_id(depth);
	int loop_size = vertex_set[loop_set_prefix_id].get_size();
	if (loop_size <= 0)
		return;
	int* loop_data_ptr = vertex_set[loop_set_prefix_id].get_data_ptr();
	/*if (clique == true)
	  {
	  int last_vertex = subtraction_set.get_last();
	// The number of this vertex must be greater than the number of last vertex.
	loop_start = std::upper_bound(loop_data_ptr, loop_data_ptr + loop_size, last_vertex) - loop_data_ptr;
	}*/
	if (depth == schedule.get_size() - 1)
	{
		// TODO : try more kinds of calculation.
		// For example, we can maintain an ordered set, but it will cost more to maintain itself when entering or exiting recursion.
		if (clique == true)
			local_ans += loop_size;
		else if (loop_size > 0)
			local_ans += VertexSet::unorderd_subtraction_size(vertex_set[loop_set_prefix_id], subtraction_set);
		return;
	}

	int last_vertex = subtraction_set.get_last();
	for (int i = 0; i < loop_size; ++i)
	{
		if (last_vertex <= loop_data_ptr[i] && clique == true)
			break;
		int vertex = loop_data_ptr[i];
		if (!clique)
			if (subtraction_set.has_data(vertex))
				continue;
		int l, r;
		get_edge_index(vertex, l, r);
		for (int prefix_id = schedule.get_last(depth); prefix_id != -1; prefix_id = schedule.get_next(prefix_id))
		{
			vertex_set[prefix_id].build_vertex_set(schedule, vertex_set, &edge[l], r - l, prefix_id, vertex, clique);
		}
		//subtraction_set.insert_ans_sort(vertex);
		subtraction_set.push_back(vertex);
		pattern_matching_func(schedule, vertex_set, subtraction_set, local_ans, depth + 1, clique);
		subtraction_set.pop_back();
	}
}

long long Graph::pattern_matching(const Schedule& schedule, int thread_count, bool clique)
{
	long long global_ans = 0;
#pragma omp parallel num_threads(thread_count) reduction(+: global_ans)
	{
		VertexSet* vertex_set = new VertexSet[schedule.get_total_prefix_num()];
		VertexSet subtraction_set;
		subtraction_set.init();
		long long local_ans = 0;
		// TODO : try different chunksize
#pragma omp for schedule(dynamic)
        for (int vertex = 0; vertex < v_cnt; ++vertex)
        {
            int l, r;
            get_edge_index(vertex, l, r);
            for (int prefix_id = schedule.get_last(0); prefix_id != -1; prefix_id = schedule.get_next(prefix_id))
            {
                vertex_set[prefix_id].build_vertex_set(schedule, vertex_set, &edge[l], r - l, prefix_id);
            }
            //subtraction_set.insert_ans_sort(vertex);
            subtraction_set.push_back(vertex);
            if (schedule.get_total_restrict_num() > 0 && clique == false)
                pattern_matching_aggressive_func(schedule, vertex_set, subtraction_set, local_ans, 1);
            else
                pattern_matching_func(schedule, vertex_set, subtraction_set, local_ans, 1, clique);
            subtraction_set.pop_back();
        }
        delete[] vertex_set;

        // TODO : Computing multiplicty for a pattern
        global_ans += local_ans;
    }
    return global_ans;
}

void Graph::pattern_matching_aggressive_func(const Schedule& schedule, VertexSet* vertex_set, VertexSet& subtraction_set, long long& local_ans, int depth)
{
    int loop_set_prefix_id = schedule.get_loop_set_prefix_id(depth);
    int loop_size = vertex_set[loop_set_prefix_id].get_size();
    if (loop_size <= 0)
        return;
    int* loop_data_ptr = vertex_set[loop_set_prefix_id].get_data_ptr();
    if (depth == schedule.get_size() - 1)
    {
        // TODO : try more kinds of calculation.
        // For example, we can maintain an ordered set, but it will cost more to maintain itself when entering or exiting recursion.
        if (schedule.get_total_restrict_num() > 0)
        {
            int min_vertex = v_cnt;
            for (int i = schedule.get_restrict_last(depth); i != -1; i = schedule.get_restrict_next(i))
                if (min_vertex > subtraction_set.get_data(schedule.get_restrict_index(i)))
                    min_vertex = subtraction_set.get_data(schedule.get_restrict_index(i));
            const VertexSet& vset = vertex_set[loop_set_prefix_id];
            int size_after_restrict = std::lower_bound(vset.get_data_ptr(), vset.get_data_ptr() + vset.get_size(), min_vertex) - vset.get_data_ptr();
            if (size_after_restrict > 0)
                local_ans += VertexSet::unorderd_subtraction_size(vertex_set[loop_set_prefix_id], subtraction_set, size_after_restrict);
        }
        else
            local_ans += VertexSet::unorderd_subtraction_size(vertex_set[loop_set_prefix_id], subtraction_set);
        return;
    }
    
    // TODO : min_vertex is also a loop invariant
    int min_vertex = v_cnt;
    for (int i = schedule.get_restrict_last(depth); i != -1; i = schedule.get_restrict_next(i))
        if (min_vertex > subtraction_set.get_data(schedule.get_restrict_index(i)))
            min_vertex = subtraction_set.get_data(schedule.get_restrict_index(i));
    for (int i = 0; i < loop_size; ++i)
    {
        if (min_vertex <= loop_data_ptr[i])
            break;
        int vertex = loop_data_ptr[i];
        if (subtraction_set.has_data(vertex))
            continue;
        int l, r;
        get_edge_index(vertex, l, r);
        for (int prefix_id = schedule.get_last(depth); prefix_id != -1; prefix_id = schedule.get_next(prefix_id))
        {
            vertex_set[prefix_id].build_vertex_set(schedule, vertex_set, &edge[l], r - l, prefix_id, vertex);
        }
        //subtraction_set.insert_ans_sort(vertex);
        subtraction_set.push_back(vertex);
        pattern_matching_aggressive_func(schedule, vertex_set, subtraction_set, local_ans, depth + 1);
        subtraction_set.pop_back();
    }
}
