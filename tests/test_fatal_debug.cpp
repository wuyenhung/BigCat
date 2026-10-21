#include "PDOFilter.h"
#include <iostream>
#include <cmath>
int main(){
    PDOFilter f;
    f.Configure(100.0, 200.0, 200.0, 1000.0, 10000);
    f.SetPDOLimits(-65.0, 65.0);
    double out=f.Filter(30.0, 0.0, 0.0);
    for(int i=0;i<420;i++){
        out=f.Filter(30.0, 0.0, 0.0);
        if(f.IsError()){
            std::cout<<"LATCH at cycle "<<i<<" out="<<out
              <<" vel="<<f.GetCurrentVelocity()
              <<" ivel="<<f.GetInternalVelocity()<<'\n';
            for(int j=0;j<100;j++){
                out=f.Filter(30.0, 0.0, 0.0);
                double vel=f.GetCurrentVelocity();
                double ivel=f.GetInternalVelocity();
                double acc=f.GetCurrentAcceleration();
                if(j%10==0 || vel>3.0)
                    std::cout<<"  +"<<j<<" out="<<out<<" vel="<<vel<<" ivel="<<ivel<<" acc="<<acc<<'\n';
                if(vel>0.1 && vel>3.0) break;
            }
            break;
        }
    }
    return 0;
}
