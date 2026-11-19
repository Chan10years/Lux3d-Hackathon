#pragma once
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

// Pure XY geometry, in centimetres. Local map: camera forward is up, right is right.
namespace CityMap
{
struct Point { double X = 0, Y = 0; };
struct Bounds { double MinX, MinY, MaxX, MaxY; };

inline Point ProjectFullMap(Point World, Bounds Area, Point Size)
{
    const double Scale = std::min(Size.X / std::max(1., Area.MaxX-Area.MinX),
                                  Size.Y / std::max(1., Area.MaxY-Area.MinY));
    return {Size.X*.5 + (World.X-(Area.MinX+Area.MaxX)*.5)*Scale,
            Size.Y*.5 + (World.Y-(Area.MinY+Area.MaxY)*.5)*Scale};
}

inline Point ProjectMiniMap(Point World, Point Centre, double CameraYaw, double WorldRadius, double PixelRadius)
{
    const double Angle = CameraYaw * 3.14159265358979323846 / 180.;
    const double X = World.X-Centre.X, Y = World.Y-Centre.Y;
    const double Scale = PixelRadius / std::max(1., WorldRadius);
    return {(-X*std::sin(Angle) + Y*std::cos(Angle))*Scale,
            (-X*std::cos(Angle) - Y*std::sin(Angle))*Scale};
}

inline Point ClampMarkerToCircle(Point P, double Radius)
{
    const double Length = std::hypot(P.X,P.Y);
    const double Scale = Length > Radius ? Radius / Length : 1.;
    return {P.X*Scale, P.Y*Scale};
}

inline bool ClipLineToCircle(Point& A, Point& B, double Radius)
{
    const double X=B.X-A.X, Y=B.Y-A.Y, Length2=X*X+Y*Y;
    if (Length2 < 1.e-12) return A.X*A.X+A.Y*A.Y <= Radius*Radius;
    const double Dot=A.X*X+A.Y*Y;
    const double D=Dot*Dot-Length2*(A.X*A.X+A.Y*A.Y-Radius*Radius);
    if (D < 0) return false;
    const double Root=std::sqrt(D);
    const double Low=std::max(0.,(-Dot-Root)/Length2);
    const double High=std::min(1.,(-Dot+Root)/Length2);
    if (Low > High) return false;
    B={A.X+X*High,A.Y+Y*High};
    A={A.X+X*Low,A.Y+Y*Low};
    return true;
}

inline bool ClipLineToRect(Point& A, Point& B, Point Size)
{
    const double DX=B.X-A.X, DY=B.Y-A.Y;
    double Low=0, High=1;
    auto Bound = [&](double P, double Q)
    {
        if (std::abs(P) < 1.e-12) return Q >= 0;
        const double T=Q/P;
        if (P < 0) Low=std::max(Low,T); else High=std::min(High,T);
        return Low <= High;
    };
    if (!Bound(-DX,A.X) || !Bound(DX,Size.X-A.X) || !Bound(-DY,A.Y) || !Bound(DY,Size.Y-A.Y)) return false;
    B={A.X+High*DX,A.Y+High*DY};
    A={A.X+Low*DX,A.Y+Low*DY};
    return true;
}

// Clip convex filled blocks to the same 48-sided disk used by the minimap.
inline std::vector<Point> ClipPolygonToCircle(std::vector<Point> Polygon, double Radius)
{
    if (Polygon.size()<3 || Radius<=0) return {};
    double MinX=Polygon[0].X, MaxX=MinX, MinY=Polygon[0].Y, MaxY=MinY;
    const double Pi=3.14159265358979323846;
    const double Inradius=Radius*std::cos(Pi/48);
    bool Inside=true;
    for (const auto P : Polygon)
    {
        MinX=std::min(MinX,P.X); MaxX=std::max(MaxX,P.X);
        MinY=std::min(MinY,P.Y); MaxY=std::max(MaxY,P.Y);
        Inside=Inside && P.X*P.X+P.Y*P.Y<=Inradius*Inradius;
    }
    if (MinX>Radius || MaxX<-Radius || MinY>Radius || MaxY<-Radius) return {};
    if (Inside) return Polygon;
    for (int I=0; I<48 && !Polygon.empty(); ++I)
    {
        const double Angle=2*Pi*(I+.5)/48;
        const double NX=std::cos(Angle), NY=std::sin(Angle);
        std::vector<Point> Clipped;
        Point Previous=Polygon.back();
        double PreviousD=Previous.X*NX+Previous.Y*NY-Inradius;
        for (const Point Current : Polygon)
        {
            const double CurrentD=Current.X*NX+Current.Y*NY-Inradius;
            if ((PreviousD<=0)!=(CurrentD<=0))
            {
                const double T=PreviousD/(PreviousD-CurrentD);
                Clipped.push_back({Previous.X+(Current.X-Previous.X)*T,Previous.Y+(Current.Y-Previous.Y)*T});
            }
            if (CurrentD<=0) Clipped.push_back(Current);
            Previous=Current;
            PreviousD=CurrentD;
        }
        Polygon=std::move(Clipped);
    }
    return Polygon;
}
}
