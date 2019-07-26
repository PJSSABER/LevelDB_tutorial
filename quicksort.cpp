#include<iostream>
#include<algorithm>
#include<vector>

using namespace std;

int partition(vector<int> *a,int low,int high)
{
    int pivi=(*a)[low];
    int cache=pivi;
    int temp;
    while(low<high)
    {
        while( low<high && (*a)[high]>=pivi) high--;
        (*a)[low]=(*a)[high];
        while( low<high && (*a)[low]<=pivi) low++;
        (*a)[high]=(*a)[low];
    }
    (*a)[low]=cache;
    return low;
}
void Qsort(vector<int> *a,int low,int high)
{
    if(low<high)
    {
        int pivotloc=partition(a,low,high);
        Qsort(a,low,pivotloc-1);
        Qsort(a,pivotloc+1,high);
    }
}

void quiksort(vector<int> *a)
{
    Qsort(a,0,(*a).size()-1);
    cout<<(*a).size()<<endl;
}



int main()
{
    vector<int> input={2,9,4,5,1,4,2,12,1,0};
    vector<int> inp=input;
    sort(input.begin(),input.end());

    for(int i=0;i<input.size();i++)
    {
        cout<<input[i];
    }
    cout<<endl;
    
    quiksort(&inp);



    for(int i=0;i<input.size();i++)
    {
        cout<<inp[i];
    }

}