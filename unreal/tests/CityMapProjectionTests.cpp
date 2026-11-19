#include "CityMapProjection.h"
#include <cassert>
#include <cstdio>
#include <vector>

static bool Near(double A, double B) { return std::abs(A - B) < .001; }
int main()
{
    using namespace CityMap;
    const Bounds B{1000, 2000, 5000, 4000};
    const auto Centre = ProjectFullMap({3000,3000}, B, {800,600});
    assert(Near(Centre.X,400) && Near(Centre.Y,300));
    const auto Corner = ProjectFullMap({1000,4000}, B, {800,600});
    assert(Near(Corner.X,0) && Near(Corner.Y,500));
    const auto Other = ProjectFullMap({5000,2000}, B, {800,600});
    assert(Near(Other.X,800) && Near(Other.Y,100));
    for (double Yaw : {0., 90., 180., 270.})
    {
        const double R = Yaw * 3.14159265358979323846 / 180.;
        const Point Origin{10000,12500};
        const auto P = ProjectMiniMap(Origin, Origin, Yaw, 10000, 100);
        assert(Near(P.X,0) && Near(P.Y,0));
        const Point Ahead{Origin.X + 5000 * std::cos(R), Origin.Y + 5000 * std::sin(R)};
        const auto F = ProjectMiniMap(Ahead, Origin, Yaw, 10000, 100);
        assert(Near(F.X,0) && Near(F.Y,-50));
        const Point Right{Origin.X - 5000 * std::sin(R), Origin.Y + 5000 * std::cos(R)};
        const auto S = ProjectMiniMap(Right, Origin, Yaw, 10000, 100);
        assert(Near(S.X,50) && Near(S.Y,0));
    }
    const auto Edge = ClampMarkerToCircle({300,400}, 100);
    assert(Near(Edge.X,60) && Near(Edge.Y,80));
    const auto In = ClampMarkerToCircle({3,4}, 100);
    assert(Near(In.X,3) && Near(In.Y,4));
    Point A{-200,0}, Z{200,0};
    assert(ClipLineToCircle(A,Z,100));
    assert(Near(A.X,-100) && Near(Z.X,100));
    A={-200,200}; Z={200,200};
    assert(!ClipLineToCircle(A,Z,100));
    A={0,0}; Z={0,0};
    assert(ClipLineToCircle(A,Z,100));
    A={-200,200}; Z={200,-200};
    assert(ClipLineToRect(A,Z,{100,100}));
    assert(Near(A.X,0) && Near(A.Y,0) && Near(Z.X,0) && Near(Z.Y,0));
    const std::vector<Point> Inside{{-10,-10},{10,-10},{10,10},{-10,10}};
    assert(ClipPolygonToCircle(Inside,100).size()==4);
    assert(ClipPolygonToCircle({{150,150},{200,150},{200,200},{150,200}},100).empty());
    const auto Covered=ClipPolygonToCircle({{-200,-200},{200,-200},{200,200},{-200,200}},100);
    assert(Covered.size()>=32);
    double TwiceArea=0;
    for (size_t I=0; I<Covered.size(); ++I)
    {
        const auto P=Covered[I], Q=Covered[(I+1)%Covered.size()];
        assert(std::hypot(P.X,P.Y)<=100.001);
        TwiceArea+=P.X*Q.Y-Q.X*P.Y;
    }
    assert(std::abs(TwiceArea)*.5>31000 && std::abs(TwiceArea)*.5<31500);
    const auto Crossing=ClipPolygonToCircle({{70,-30},{130,-30},{130,30},{70,30}},100);
    assert(Crossing.size()>=4);
    for (const auto P : Crossing) assert(std::hypot(P.X,P.Y)<=100.001 && P.X>=69.999);
    puts("City map projection and filled polygon clipping passed");
}
