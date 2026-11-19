#include "BeaconProceduralMesh.h"
#include "ProceduralMeshComponent.h"

namespace BeaconProceduralMeshDetail
{
	struct FBeaconMeshBuffers
	{
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FLinearColor> VertexColors;
		TArray<FProcMeshTangent> Tangents;

		void Reserve(int32 NumVertices, int32 NumTriangles)
		{
			Vertices.Reserve(NumVertices);
			Triangles.Reserve(NumTriangles * 3);
			Normals.Reserve(NumVertices);
			UVs.Reserve(NumVertices);
			VertexColors.Reserve(NumVertices);
			Tangents.Reserve(NumVertices);
		}

		int32 AddVertex(
			const FVector& Position,
			const FVector& Normal,
			const FVector2D& UV,
			const FLinearColor& Color,
			const FProcMeshTangent& Tangent)
		{
			const int32 Index = Vertices.Add(Position);
			Normals.Add(Normal);
			UVs.Add(UV);
			VertexColors.Add(Color);
			Tangents.Add(Tangent);
			return Index;
		}

		void AddTriangle(int32 Index0, int32 Index1, int32 Index2)
		{
			Triangles.Add(Index0);
			Triangles.Add(Index1);
			Triangles.Add(Index2);
		}
	};

	struct FCrystalRingSpec
	{
		double Z;
		double Radius;
		FLinearColor Color;
	};

	static FProcMeshTangent MakeSafeTangent(const FVector& Normal, const FVector& PreferredU)
	{
		const FVector SafeNormal = Normal.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		FVector Tangent = PreferredU - SafeNormal * FVector::DotProduct(PreferredU, SafeNormal);
		if (Tangent.SizeSquared() <= 1e-8)
		{
			const FVector FallbackAxis = (FMath::Abs(SafeNormal.Z) < 0.9) ? FVector::UpVector : FVector::ForwardVector;
			Tangent = FVector::CrossProduct(FallbackAxis, SafeNormal);
		}
		Tangent = Tangent.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::ForwardVector);
		return FProcMeshTangent(Tangent, false);
	}

	/**
	 * Emits a non-shared flat-shaded triangle whose winding matches UE front-face convention
	 * ((P2 - P0) x (P1 - P0) aligned with OutwardHint).
	 */
	static void AddFlatTriangle(
		FBeaconMeshBuffers& Mesh,
		const FVector& P0,
		const FVector& P1,
		const FVector& P2,
		const FVector2D& UV0,
		const FVector2D& UV1,
		const FVector2D& UV2,
		const FLinearColor& Color0,
		const FLinearColor& Color1,
		const FLinearColor& Color2,
		const FVector& OutwardHint)
	{
		FVector V0 = P0;
		FVector V1 = P1;
		FVector V2 = P2;
		FVector2D T0 = UV0;
		FVector2D T1 = UV1;
		FVector2D T2 = UV2;
		FLinearColor C0 = Color0;
		FLinearColor C1 = Color1;
		FLinearColor C2 = Color2;

		FVector FaceCross = FVector::CrossProduct(V2 - V0, V1 - V0);
		if (FVector::DotProduct(FaceCross, OutwardHint) < 0.0)
		{
			Swap(V1, V2);
			Swap(T1, T2);
			Swap(C1, C2);
			FaceCross = -FaceCross;
		}

		const FVector FallbackNormal = OutwardHint.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
		const FVector FlatNormal = FaceCross.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FallbackNormal);
		const FProcMeshTangent Tangent = MakeSafeTangent(FlatNormal, V1 - V0);

		const int32 BaseIndex = Mesh.AddVertex(V0, FlatNormal, T0, C0, Tangent);
		Mesh.AddVertex(V1, FlatNormal, T1, C1, Tangent);
		Mesh.AddVertex(V2, FlatNormal, T2, C2, Tangent);
		Mesh.AddTriangle(BaseIndex, BaseIndex + 1, BaseIndex + 2);
	}

	/**
	 * Appends a double-terminated N-sided faceted crystal into Mesh using non-shared flat-shaded triangles.
	 */
	static void AppendFacetedCrystal(
		FBeaconMeshBuffers& Mesh,
		const FTransform& LocalToWorld,
		int32 NumSides,
		double BottomTipZ,
		const FLinearColor& BottomTipColor,
		TArrayView<const FCrystalRingSpec> Rings,
		double TopApexZ,
		const FLinearColor& TopApexColor)
	{
		if (NumSides < 3 || Rings.Num() == 0)
		{
			return;
		}

		const double HeightSpan = FMath::Max(1.0, TopApexZ - BottomTipZ);
		auto ComputeV = [BottomTipZ, HeightSpan](double LocalZ) -> double
		{
			return (LocalZ - BottomTipZ) / HeightSpan;
		};

		const FVector CrystalCenter = LocalToWorld.GetLocation();
		const FVector BottomTip = LocalToWorld.TransformPosition(FVector(0.0, 0.0, BottomTipZ));
		const FVector TopApex = LocalToWorld.TransformPosition(FVector(0.0, 0.0, TopApexZ));

		auto TransformRingPoint = [&LocalToWorld](double Angle, const FCrystalRingSpec& Spec) -> FVector
		{
			const FVector LocalPos(Spec.Radius * FMath::Cos(Angle), Spec.Radius * FMath::Sin(Angle), Spec.Z);
			return LocalToWorld.TransformPosition(LocalPos);
		};

		for (int32 Side = 0; Side < NumSides; ++Side)
		{
			const double U0 = static_cast<double>(Side) / static_cast<double>(NumSides);
			const double U1 = static_cast<double>(Side + 1) / static_cast<double>(NumSides);
			const double UMid = 0.5 * (U0 + U1);
			const double Angle0 = U0 * UE_DOUBLE_TWO_PI;
			const double Angle1 = U1 * UE_DOUBLE_TWO_PI;

			// Lower termination cone
			{
				const FCrystalRingSpec& FirstRing = Rings[0];
				const double VFirst = ComputeV(FirstRing.Z);
				const FVector R0 = TransformRingPoint(Angle0, FirstRing);
				const FVector R1 = TransformRingPoint(Angle1, FirstRing);
				const FVector Outward = ((BottomTip + R0 + R1) / 3.0) - CrystalCenter;

				AddFlatTriangle(
					Mesh,
					BottomTip, R0, R1,
					FVector2D(UMid, 0.0), FVector2D(U0, VFirst), FVector2D(U1, VFirst),
					BottomTipColor, FirstRing.Color, FirstRing.Color,
					Outward);
			}

			// Intermediate faceted bands between consecutive rings
			for (int32 RingIdx = 0; RingIdx + 1 < Rings.Num(); ++RingIdx)
			{
				const FCrystalRingSpec& Lower = Rings[RingIdx];
				const FCrystalRingSpec& Upper = Rings[RingIdx + 1];
				const double VLower = ComputeV(Lower.Z);
				const double VUpper = ComputeV(Upper.Z);

				const FVector Bot0 = TransformRingPoint(Angle0, Lower);
				const FVector Bot1 = TransformRingPoint(Angle1, Lower);
				const FVector Top0 = TransformRingPoint(Angle0, Upper);
				const FVector Top1 = TransformRingPoint(Angle1, Upper);

				const FVector OutwardA = ((Bot0 + Top0 + Top1) / 3.0) - CrystalCenter;
				AddFlatTriangle(
					Mesh,
					Bot0, Top0, Top1,
					FVector2D(U0, VLower), FVector2D(U0, VUpper), FVector2D(U1, VUpper),
					Lower.Color, Upper.Color, Upper.Color,
					OutwardA);

				const FVector OutwardB = ((Bot0 + Top1 + Bot1) / 3.0) - CrystalCenter;
				AddFlatTriangle(
					Mesh,
					Bot0, Top1, Bot1,
					FVector2D(U0, VLower), FVector2D(U1, VUpper), FVector2D(U1, VLower),
					Lower.Color, Upper.Color, Lower.Color,
					OutwardB);
			}

			// Upper termination crown
			{
				const FCrystalRingSpec& LastRing = Rings.Last();
				const double VLast = ComputeV(LastRing.Z);
				const FVector R0 = TransformRingPoint(Angle0, LastRing);
				const FVector R1 = TransformRingPoint(Angle1, LastRing);
				const FVector Outward = ((R0 + TopApex + R1) / 3.0) - CrystalCenter;

				AddFlatTriangle(
					Mesh,
					R0, TopApex, R1,
					FVector2D(U0, VLast), FVector2D(UMid, 1.0), FVector2D(U1, VLast),
					LastRing.Color, TopApexColor, LastRing.Color,
					Outward);
			}
		}
	}

	/**
	 * Appends a flat ground-plane ring at height Z with upward (0,0,1) normals.
	 */
	static void AppendGroundRing(
		FBeaconMeshBuffers& Mesh,
		int32 Segments,
		double Z,
		double InnerRadius,
		double OuterRadius,
		const FLinearColor& InnerColor,
		const FLinearColor& OuterColor)
	{
		if (Segments < 3 || OuterRadius <= InnerRadius)
		{
			return;
		}

		const int32 BaseVertex = Mesh.Vertices.Num();
		const FVector UpNormal(0.0, 0.0, 1.0);

		for (int32 SegIdx = 0; SegIdx <= Segments; ++SegIdx)
		{
			const double U = static_cast<double>(SegIdx) / static_cast<double>(Segments);
			const double Theta = U * UE_DOUBLE_TWO_PI;
			const double CosT = FMath::Cos(Theta);
			const double SinT = FMath::Sin(Theta);

			const FVector Radial(CosT, SinT, 0.0);
			const FVector CircumTangent(-SinT, CosT, 0.0);
			const FProcMeshTangent Tangent = MakeSafeTangent(UpNormal, CircumTangent);

			Mesh.AddVertex(
				FVector(Radial.X * InnerRadius, Radial.Y * InnerRadius, Z),
				UpNormal,
				FVector2D(U, 0.0),
				InnerColor,
				Tangent);

			Mesh.AddVertex(
				FVector(Radial.X * OuterRadius, Radial.Y * OuterRadius, Z),
				UpNormal,
				FVector2D(U, 1.0),
				OuterColor,
				Tangent);
		}

		for (int32 SegIdx = 0; SegIdx < Segments; ++SegIdx)
		{
			const int32 In0 = BaseVertex + SegIdx * 2;
			const int32 Out0 = In0 + 1;
			const int32 In1 = In0 + 2;
			const int32 Out1 = In0 + 3;

			// Outward normal check: (Out0 - In0) x (Out1 - In0) = +Z; UE front-face uses (C-A) x (B-A)
			Mesh.AddTriangle(In0, Out1, Out0);
			Mesh.AddTriangle(In0, In1, Out1);
		}
	}

	/**
	 * Appends a smooth vertical hollow frustum with quadratic alpha falloff toward the top.
	 */
	static void AppendVerticalFrustum(
		FBeaconMeshBuffers& Mesh,
		int32 RadialSegments,
		int32 HeightSubdivs,
		double BaseZ,
		double TopZ,
		double BottomRadius,
		double TopRadius,
		const FLinearColor& BottomRGB,
		const FLinearColor& TopRGB,
		float BaseAlpha)
	{
		if (RadialSegments < 3 || HeightSubdivs < 1 || TopZ <= BaseZ)
		{
			return;
		}

		const double TotalHeight = TopZ - BaseZ;
		const double RadiusSlope = BottomRadius - TopRadius;
		const int32 Stride = RadialSegments + 1;
		const int32 BaseVertex = Mesh.Vertices.Num();

		for (int32 Row = 0; Row <= HeightSubdivs; ++Row)
		{
			const double HeightFrac = static_cast<double>(Row) / static_cast<double>(HeightSubdivs);
			const float HeightFracF = static_cast<float>(HeightFrac);
			const float Fade = 1.0f - HeightFracF;
			const float Alpha = BaseAlpha * Fade * Fade;

			const FLinearColor RowColor = FMath::Lerp(BottomRGB, TopRGB, HeightFracF);
			const FLinearColor FinalColor(RowColor.R, RowColor.G, RowColor.B, Alpha);

			const double Z = FMath::Lerp(BaseZ, TopZ, HeightFrac);
			const double RingRadius = FMath::Lerp(BottomRadius, TopRadius, HeightFrac);

			for (int32 Seg = 0; Seg <= RadialSegments; ++Seg)
			{
				const double U = static_cast<double>(Seg) / static_cast<double>(RadialSegments);
				const double Theta = U * UE_DOUBLE_TWO_PI;
				const double CosT = FMath::Cos(Theta);
				const double SinT = FMath::Sin(Theta);

				const FVector Radial(CosT, SinT, 0.0);
				const FVector CircumTangent(-SinT, CosT, 0.0);
				const FVector Normal = (TotalHeight * Radial + RadiusSlope * FVector::UpVector)
					.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, Radial);
				const FProcMeshTangent Tangent = MakeSafeTangent(Normal, CircumTangent);

				Mesh.AddVertex(
					FVector(Radial.X * RingRadius, Radial.Y * RingRadius, Z),
					Normal,
					FVector2D(U, HeightFrac),
					FinalColor,
					Tangent);
			}
		}

		for (int32 Row = 0; Row < HeightSubdivs; ++Row)
		{
			for (int32 Seg = 0; Seg < RadialSegments; ++Seg)
			{
				const int32 Bot0 = BaseVertex + Row * Stride + Seg;
				const int32 Bot1 = Bot0 + 1;
				const int32 Top0 = Bot0 + Stride;
				const int32 Top1 = Top0 + 1;

				Mesh.AddTriangle(Bot0, Top0, Top1);
				Mesh.AddTriangle(Bot0, Top1, Bot1);
			}
		}
	}

	/**
	 * Appends a two-sided thin vertical energy rib fin radiating along FinAngle.
	 */
	static void AppendVerticalEnergyFin(
		FBeaconMeshBuffers& Mesh,
		double FinAngleRad,
		int32 HeightSubdivs,
		double BaseZ,
		double TopZ,
		double InnerRadius,
		double BottomOuterRadius,
		double TopOuterRadius,
		double HalfThickness,
		const FLinearColor& BaseColor,
		const FLinearColor& TopColor,
		float BaseAlpha)
	{
		if (HeightSubdivs < 1 || TopZ <= BaseZ)
		{
			return;
		}

		const FVector RadialDir(FMath::Cos(FinAngleRad), FMath::Sin(FinAngleRad), 0.0);
		// Cross(Up, RadialDir) = (-Sin, Cos, 0)
		const FVector SideNormal(-RadialDir.Y, RadialDir.X, 0.0);

		for (int32 SideSign = -1; SideSign <= 1; SideSign += 2)
		{
			const double SignD = static_cast<double>(SideSign);
			const FVector FaceNormal = SideNormal * SignD;
			const FVector SideOffset = SideNormal * (HalfThickness * SignD);
			const FProcMeshTangent Tangent = MakeSafeTangent(FaceNormal, RadialDir);

			const int32 BaseVertex = Mesh.Vertices.Num();
			for (int32 Row = 0; Row <= HeightSubdivs; ++Row)
			{
				const double HeightFrac = static_cast<double>(Row) / static_cast<double>(HeightSubdivs);
				const float HeightFracF = static_cast<float>(HeightFrac);
				const float Fade = 1.0f - HeightFracF;
				const float Alpha = BaseAlpha * Fade * Fade;

				const FLinearColor RowColor = FMath::Lerp(BaseColor, TopColor, HeightFracF);
				const FLinearColor FinalColor(RowColor.R, RowColor.G, RowColor.B, Alpha);

				const double Z = FMath::Lerp(BaseZ, TopZ, HeightFrac);
				const double OuterRadius = FMath::Lerp(BottomOuterRadius, TopOuterRadius, HeightFrac);

				Mesh.AddVertex(
					RadialDir * InnerRadius + FVector(0.0, 0.0, Z) + SideOffset,
					FaceNormal,
					FVector2D(0.0, HeightFrac),
					FinalColor,
					Tangent);

				Mesh.AddVertex(
					RadialDir * OuterRadius + FVector(0.0, 0.0, Z) + SideOffset,
					FaceNormal,
					FVector2D(1.0, HeightFrac),
					FinalColor,
					Tangent);
			}

			for (int32 Row = 0; Row < HeightSubdivs; ++Row)
			{
				const int32 InBot = BaseVertex + Row * 2;
				const int32 OutBot = InBot + 1;
				const int32 InTop = InBot + 2;
				const int32 OutTop = InBot + 3;

				if (SideSign > 0)
				{
					// (OutTop - InBot) x (OutBot - InBot) aligns with +SideNormal
					Mesh.AddTriangle(InBot, OutBot, OutTop);
					Mesh.AddTriangle(InBot, OutTop, InTop);
				}
				else
				{
					// Reversed winding aligns with -SideNormal
					Mesh.AddTriangle(InBot, OutTop, OutBot);
					Mesh.AddTriangle(InBot, InTop, OutTop);
				}
			}
		}
	}

	static void CommitSectionZero(UProceduralMeshComponent* Target, const FBeaconMeshBuffers& Mesh)
	{
		if (!Target)
		{
			return;
		}

		Target->bUseAsyncCooking = true;
		Target->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Target->SetCastShadow(false);
		Target->CreateMeshSection_LinearColor(
			0,
			Mesh.Vertices,
			Mesh.Triangles,
			Mesh.Normals,
			Mesh.UVs,
			Mesh.VertexColors,
			Mesh.Tangents,
			/*bCreateCollision=*/false);
	}
}

namespace BeaconProceduralMesh
{
	using namespace BeaconProceduralMeshDetail;

	void BuildCoreCrystal(UProceduralMeshComponent* Target)
	{
		if (!Target)
		{
			return;
		}

		const FLinearColor BrightCyanWhite(0.55f, 1.0f, 1.0f, 0.95f);
		const FLinearColor DeepElectricTeal(0.08f, 0.72f, 0.98f, 0.95f);

		const FCrystalRingSpec Rings[] =
		{
			{ -30.0, 16.0, DeepElectricTeal },
			{ -8.0,  24.0, BrightCyanWhite },
			{  8.0,  24.0, BrightCyanWhite },
			{  30.0, 17.0, DeepElectricTeal }
		};

		// 8 sides: 8 lower cap + 24*2 side tris + 8 upper cap = 64 triangles, 192 vertices
		FBeaconMeshBuffers Mesh;
		Mesh.Reserve(192, 64);

		AppendFacetedCrystal(
			Mesh,
			FTransform::Identity,
			8,
			-56.0,
			BrightCyanWhite,
			Rings,
			56.0,
			BrightCyanWhite);

		CommitSectionZero(Target, Mesh);
	}

	void BuildOrbitShards(UProceduralMeshComponent* Target)
	{
		if (!Target)
		{
			return;
		}

		constexpr int32 NumShards = 4;
		constexpr int32 NumSides = 6;
		constexpr double OrbitRadius = 48.0;
		const double TiltRad = FMath::DegreesToRadians(12.0);

		const FLinearColor ShardApexColor(0.60f, 1.0f, 0.88f, 0.95f);
		const FLinearColor ShardUpperColor(0.35f, 1.0f, 0.78f, 0.95f);
		const FLinearColor ShardLowerColor(0.12f, 0.92f, 0.68f, 0.95f);
		const FLinearColor ShardTipColor(0.18f, 0.96f, 0.85f, 0.95f);

		const FCrystalRingSpec ShardRings[] =
		{
			{ -3.5, 7.0, ShardLowerColor },
			{  4.5, 6.2, ShardUpperColor }
		};

		// Per hexagonal shard: 6 bottom + 12 side + 6 top = 24 tris (72 verts) -> 4 shards = 96 tris, 288 verts
		FBeaconMeshBuffers Mesh;
		Mesh.Reserve(288, 96);

		for (int32 ShardIdx = 0; ShardIdx < NumShards; ++ShardIdx)
		{
			const double OrbitAngle = (static_cast<double>(ShardIdx) / static_cast<double>(NumShards)) * UE_DOUBLE_TWO_PI;
			const FVector RadialDir(FMath::Cos(OrbitAngle), FMath::Sin(OrbitAngle), 0.0);
			const FVector TangentDir(-RadialDir.Y, RadialDir.X, 0.0);

			// Tilt shard local +Z axis inward toward the core crystal at origin
			const FVector LocalZ = (-RadialDir * FMath::Sin(TiltRad) + FVector::UpVector * FMath::Cos(TiltRad))
				.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, FVector::UpVector);
			const FVector LocalY = TangentDir;
			const FVector LocalX = FVector::CrossProduct(LocalY, LocalZ)
				.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, RadialDir);

			const FMatrix BasisMatrix(LocalX, LocalY, LocalZ, RadialDir * OrbitRadius);
			const FTransform ShardTransform(BasisMatrix);

			AppendFacetedCrystal(
				Mesh,
				ShardTransform,
				NumSides,
				-13.0,
				ShardTipColor,
				ShardRings,
				13.0,
				ShardApexColor);
		}

		CommitSectionZero(Target, Mesh);
	}

	void BuildCalibrationRing(UProceduralMeshComponent* Target, float Radius, float Thickness, const FLinearColor& Tint)
	{
		if (!Target)
		{
			return;
		}

		constexpr int32 MajorSegments = 24;
		constexpr int32 MinorSegments = 8;

		const double SafeRadius = FMath::IsFinite(Radius) ? FMath::Max(1.0, static_cast<double>(Radius)) : 56.0;
		const double SafeThickness = FMath::IsFinite(Thickness)
			? FMath::Clamp(static_cast<double>(Thickness), 0.1, SafeRadius * 0.45)
			: 2.4;

		const FLinearColor SafeTint(
			FMath::IsFinite(Tint.R) ? Tint.R : 1.0f,
			FMath::IsFinite(Tint.G) ? Tint.G : 1.0f,
			FMath::IsFinite(Tint.B) ? Tint.B : 1.0f,
			FMath::IsFinite(Tint.A) ? Tint.A : 1.0f);

		FBeaconMeshBuffers Mesh;
		Mesh.Reserve((MajorSegments + 1) * (MinorSegments + 1), MajorSegments * MinorSegments * 2);

		const int32 MinorStride = MinorSegments + 1;

		for (int32 MajorIdx = 0; MajorIdx <= MajorSegments; ++MajorIdx)
		{
			const double U = static_cast<double>(MajorIdx) / static_cast<double>(MajorSegments);
			const double Theta = U * UE_DOUBLE_TWO_PI;
			const double CosT = FMath::Cos(Theta);
			const double SinT = FMath::Sin(Theta);

			const FVector Radial(CosT, SinT, 0.0);
			const FVector RingCenter = Radial * SafeRadius;
			const FVector CircumTangent(-SinT, CosT, 0.0);

			for (int32 MinorIdx = 0; MinorIdx <= MinorSegments; ++MinorIdx)
			{
				const double V = static_cast<double>(MinorIdx) / static_cast<double>(MinorSegments);
				const double Phi = V * UE_DOUBLE_TWO_PI;

				const FVector OutwardNormal = (Radial * FMath::Cos(Phi) + FVector::UpVector * FMath::Sin(Phi))
					.GetSafeNormal(UE_DOUBLE_SMALL_NUMBER, Radial);
				const FVector VertexPos = RingCenter + OutwardNormal * SafeThickness;
				const FProcMeshTangent Tangent = MakeSafeTangent(OutwardNormal, CircumTangent);

				Mesh.AddVertex(VertexPos, OutwardNormal, FVector2D(U, V), SafeTint, Tangent);
			}
		}

		for (int32 MajorIdx = 0; MajorIdx < MajorSegments; ++MajorIdx)
		{
			for (int32 MinorIdx = 0; MinorIdx < MinorSegments; ++MinorIdx)
			{
				const int32 P00 = MajorIdx * MinorStride + MinorIdx;
				const int32 P01 = P00 + 1;
				const int32 P10 = P00 + MinorStride;
				const int32 P11 = P10 + 1;

				Mesh.AddTriangle(P00, P01, P11);
				Mesh.AddTriangle(P00, P11, P10);
			}
		}

		CommitSectionZero(Target, Mesh);
	}

	void BuildGroundHalo(UProceduralMeshComponent* Target, float PickupRadius)
	{
		if (!Target)
		{
			return;
		}

		constexpr int32 OuterSegments = 36;
		constexpr int32 InnerHexSegments = 6;
		constexpr double GroundHeight = 2.0;

		const double SafePickupRadius = FMath::IsFinite(PickupRadius)
			? FMath::Max(20.0, static_cast<double>(PickupRadius))
			: 130.0;

		const double HexInnerR = 0.58 * SafePickupRadius;
		const double HexOuterR = 0.63 * SafePickupRadius;
		const double AnnulusOuterR = SafePickupRadius;
		const double AnnulusInnerR = FMath::Max(HexOuterR + 1.0, SafePickupRadius - 12.0);

		const FLinearColor HaloInnerGold(1.0f, 0.84f, 0.28f, 0.72f);
		const FLinearColor HaloOuterCyan(0.18f, 0.94f, 1.0f, 0.92f);
		const FLinearColor HexInnerCyan(0.32f, 1.0f, 0.94f, 0.85f);
		const FLinearColor HexOuterGold(1.0f, 0.86f, 0.34f, 0.85f);

		FBeaconMeshBuffers Mesh;
		Mesh.Reserve((OuterSegments + 1) * 2 + (InnerHexSegments + 1) * 2, (OuterSegments + InnerHexSegments) * 2);

		AppendGroundRing(
			Mesh,
			OuterSegments,
			GroundHeight,
			AnnulusInnerR,
			AnnulusOuterR,
			HaloInnerGold,
			HaloOuterCyan);

		AppendGroundRing(
			Mesh,
			InnerHexSegments,
			GroundHeight,
			HexInnerR,
			HexOuterR,
			HexInnerCyan,
			HexOuterGold);

		CommitSectionZero(Target, Mesh);
	}

	void BuildLightPillar(UProceduralMeshComponent* Target, float PillarHeight)
	{
		if (!Target)
		{
			return;
		}

		constexpr int32 RadialSegments = 20;
		constexpr int32 HeightSubdivs = 12;
		constexpr int32 FinCount = 4;
		constexpr int32 FinHeightSubdivs = 8;
		constexpr double BaseZ = -20.0;

		const double SafeTopZ = FMath::IsFinite(PillarHeight)
			? FMath::Max(100.0, static_cast<double>(PillarHeight))
			: 2800.0;
		const double FinTopZ = FMath::Max(BaseZ + 40.0, 0.65 * SafeTopZ);

		const int32 FrustumVerts = (RadialSegments + 1) * (HeightSubdivs + 1);
		const int32 FrustumTris = RadialSegments * HeightSubdivs * 2;
		const int32 FinVerts = FinCount * 2 * (FinHeightSubdivs + 1) * 2;
		const int32 FinTris = FinCount * 2 * FinHeightSubdivs * 2;

		FBeaconMeshBuffers Mesh;
		Mesh.Reserve(FrustumVerts * 2 + FinVerts, FrustumTris * 2 + FinTris);

		// Layer A: Inner Core Beam (15 cm -> 8 cm)
		AppendVerticalFrustum(
			Mesh,
			RadialSegments,
			HeightSubdivs,
			BaseZ,
			SafeTopZ,
			15.0,
			8.0,
			FLinearColor(0.70f, 1.0f, 1.0f, 0.90f),
			FLinearColor(0.25f, 0.92f, 1.0f, 0.0f),
			0.90f);

		// Layer B: Outer Ethereal Aura (38 cm -> 64 cm)
		AppendVerticalFrustum(
			Mesh,
			RadialSegments,
			HeightSubdivs,
			BaseZ,
			SafeTopZ,
			38.0,
			64.0,
			FLinearColor(0.15f, 0.80f, 1.0f, 0.42f),
			FLinearColor(0.08f, 0.55f, 0.98f, 0.0f),
			0.42f);

		// Layer C: 4 Vertical Energy Ribs at 0, 90, 180, 270 degrees
		const FLinearColor RibBaseColor(0.45f, 0.98f, 1.0f, 0.65f);
		const FLinearColor RibTopColor(0.12f, 0.75f, 1.0f, 0.0f);
		for (int32 FinIdx = 0; FinIdx < FinCount; ++FinIdx)
		{
			const double FinAngle = (static_cast<double>(FinIdx) / static_cast<double>(FinCount)) * UE_DOUBLE_TWO_PI;
			AppendVerticalEnergyFin(
				Mesh,
				FinAngle,
				FinHeightSubdivs,
				BaseZ,
				FinTopZ,
				4.0,
				48.0,
				14.0,
				0.35,
				RibBaseColor,
				RibTopColor,
				0.65f);
		}

		CommitSectionZero(Target, Mesh);
	}
}
