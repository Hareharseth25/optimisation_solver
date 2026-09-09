// LP battery incl. degenerate/equality/free-variable cases, emitted for
// comparison against scipy/HiGHS.
#include "solver/orchestrator.h"
#include <cstdio>
#include <limits>
#include <random>
static const double INF=std::numeric_limits<double>::infinity();
int main(){
    std::mt19937 g(13579);
    std::uniform_real_distribution<double> U(-4,4);
    const int N=250;
    printf("%d\n",N);
    for(int t=0;t<N;++t){
        const int n=2+(int)(g()%6), m=1+(int)(g()%6);
        model::Model mm;
        std::vector<double> lo(n),hi(n);
        for(int j=0;j<n;++j){model::Variable v;v.name="x"+std::to_string(j);
            const int kind=g()%4;
            // include free variables and one-sided bounds
            lo[j] = (kind==0)?-INF:((kind==1)?0.0:-6.0);
            hi[j] = (kind==0)?INF:((kind==2)?INF:8.0);
            v.lowerBound=lo[j];v.upperBound=hi[j];
            v.type=model::VariableType::Continuous;mm.variables.push_back(v);}
        std::vector<std::vector<double>> A(m,std::vector<double>(n,0.0));
        std::vector<double> rl(m),ru(m);
        const bool degenerate = (t%5==0);
        for(int i=0;i<m;++i){model::Constraint c;c.name="r"+std::to_string(i);
            if (degenerate && i>0){ for(int j=0;j<n;++j) A[i][j]=A[0][j]; }  // duplicate row
            else { for(int j=0;j<n;++j) if(g()%2) A[i][j]=U(g); }
            for(int j=0;j<n;++j) if(A[i][j]!=0.0) c.linearTerms.push_back({j,A[i][j]});
            const int kind=g()%4; const double b=4+std::abs(U(g));
            if(kind==0){rl[i]=b;ru[i]=b;}            // equality
            else if(kind==1){rl[i]=-INF;ru[i]=b;}
            else if(kind==2){rl[i]=-b;ru[i]=INF;}
            else {rl[i]=-b;ru[i]=b;}
            c.lowerBound=rl[i];c.upperBound=ru[i];mm.constraints.push_back(c);}
        for(int j=0;j<n;++j) mm.objective.linearTerms.push_back({j,U(g)});
        mm.objective.sense=(g()%2)?model::ObjectiveSense::Maximize:model::ObjectiveSense::Minimize;
        mm.objective.offset=U(g);
        auto r = solver::solve(mm);
        printf("%d %d %d %.17g %d\n",n,m,
            mm.objective.sense==model::ObjectiveSense::Maximize?1:0,
            mm.objective.offset, degenerate?1:0);
        for(int j=0;j<n;++j) printf("%.17g %.17g ",lo[j],hi[j]); printf("\n");
        for(const auto&t2:mm.objective.linearTerms) printf("%d %.17g ",t2.variableIndex,t2.value);
        printf("\n");
        for(int i=0;i<m;++i){for(int j=0;j<n;++j) printf("%.17g ",A[i][j]);
            printf("%.17g %.17g\n",rl[i],ru[i]);}
        printf("%s %.17g\n", solver::toString(r.status), r.objectiveValue);
    }
    return 0;
}
