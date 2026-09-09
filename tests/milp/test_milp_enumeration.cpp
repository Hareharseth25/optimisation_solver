// Section 6: independent ground truth for small MILPs by exhaustive enumeration.
#include "solver/orchestrator.h"
#include <cstdio>
#include <limits>
#include <random>
#include <vector>
static const double INF=std::numeric_limits<double>::infinity();
int main(){
    std::mt19937 g(24680);
    std::uniform_real_distribution<double> U(-5,5);
    int cases=0, agree=0, disagree=0, other=0;
    for (int t=0;t<200;++t){
        const int n=2+(int)(g()%8);           // up to 9 binaries -> 512 subsets
        const int m=1+(int)(g()%3);
        model::Model mm;
        for(int j=0;j<n;++j){model::Variable v;v.name="b"+std::to_string(j);
            v.lowerBound=0;v.upperBound=1;v.type=model::VariableType::Binary;mm.variables.push_back(v);}
        std::vector<std::vector<double>> A(m,std::vector<double>(n,0.0));
        std::vector<double> rhs(m);
        for(int i=0;i<m;++i){model::Constraint c;c.name="r"+std::to_string(i);
            for(int j=0;j<n;++j){double a=std::abs(U(g));A[i][j]=a;c.linearTerms.push_back({j,a});}
            rhs[i]=std::abs(U(g))*n*0.35+1.0;
            c.lowerBound=-INF;c.upperBound=rhs[i];mm.constraints.push_back(c);}
        std::vector<double> c(n);
        for(int j=0;j<n;++j){c[j]=U(g);mm.objective.linearTerms.push_back({j,c[j]});}
        const bool maxi = (g()%2)==0;
        mm.objective.sense = maxi?model::ObjectiveSense::Maximize:model::ObjectiveSense::Minimize;
        const double off = U(g); mm.objective.offset = off;

        // exhaustive enumeration
        double best = maxi ? -INF : INF; bool feasibleExists=false;
        for(long mask=0; mask<(1L<<n); ++mask){
            bool ok=true;
            for(int i=0;i<m && ok;++i){double a=0;
                for(int j=0;j<n;++j) if(mask>>j&1) a+=A[i][j];
                if(a>rhs[i]+1e-9) ok=false;}
            if(!ok) continue;
            feasibleExists=true;
            double v=off; for(int j=0;j<n;++j) if(mask>>j&1) v+=c[j];
            if(maxi){ if(v>best) best=v; } else { if(v<best) best=v; }
        }
        auto r = solver::solve(mm);
        ++cases;
        if (r.status==solver::SolveStatus::Optimal && feasibleExists){
            if (std::abs(r.objectiveValue-best) <= 1e-6*(1+std::abs(best))) ++agree;
            else { ++disagree; printf("  t=%d MILP=%.9g enum=%.9g\n",t,r.objectiveValue,best); }
        } else if (r.status==solver::SolveStatus::Infeasible && !feasibleExists){
            ++agree;
        } else { ++other; printf("  t=%d status=%s feasibleExists=%d enum=%.9g\n",
                    t, solver::toString(r.status), (int)feasibleExists, best); }
    }
    printf("\n%d small MILPs vs exhaustive enumeration: %d agree, %d disagree, %d other\n",
        cases, agree, disagree, other);
    return (disagree||other)?1:0;
}
