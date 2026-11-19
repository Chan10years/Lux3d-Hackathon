#include "TrioCombatAuthoring.h"
#include "TrioAtlasBake.h"
#include "TrioCombat.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Algo/Find.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsAssetUtils.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "K2Node_InputKey.h"
#include "K2Node_VariableSet.h"
#include "K2Node_Self.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/SavePackage.h"

class FTrioCombatEditorModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override { TrioAtlasBake::Startup(); }
    virtual void ShutdownModule() override { TrioAtlasBake::Shutdown(); }
};

IMPLEMENT_MODULE(FTrioCombatEditorModule, TrioCombatEditor)

namespace
{
    bool SaveAsset(UObject& Asset)
    {
        UPackage* Package = Asset.GetOutermost();
        const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.Error = GWarn;
        return UPackage::SavePackage(Package, &Asset, *File, Args);
    }

    UDataTable* MakeTable(const TCHAR* Name, UScriptStruct* Row, const TCHAR* Csv)
    {
        const FString PackagePath = FString(TEXT("/TrioCombat/")) + Name;
        const FString ObjectPath = PackagePath + TEXT(".") + Name;
        if (UDataTable* Existing = LoadObject<UDataTable>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn))
            return Existing;
        FString Data;
        const FString Path = FPaths::ProjectPluginsDir() / TEXT("TrioCombat/Data") / Csv;
        if (!FFileHelper::LoadFileToString(Data, *Path))
            return nullptr;

        UPackage* Package = CreatePackage(*PackagePath);
        UDataTable* Table = NewObject<UDataTable>(Package, Name, RF_Public | RF_Standalone | RF_Transactional);
        Table->RowStruct = Row;
        const TArray<FString> Problems = Table->CreateTableFromCSVString(Data);
        for (const FString& Problem : Problems)
            UE_LOG(LogTemp, Error, TEXT("TrioCombat %s: %s"), Name, *Problem);
        if (!Problems.IsEmpty())
            return nullptr;
        FAssetRegistryModule::AssetCreated(Table);
        Package->MarkPackageDirty();
        return SaveAsset(*Table) ? Table : nullptr;
    }

    UEdGraph* Graph(UBlueprint& Blueprint, FName Name)
    {
        TArray<UEdGraph*> Graphs;
        Blueprint.GetAllGraphs(Graphs);
        for (UEdGraph* Item : Graphs)
            if (Item->GetFName() == Name)
                return Item;
        return nullptr;
    }

    UEdGraphNode* Node(UEdGraph& Graph, FName Name)
    {
        const auto* Found = Algo::FindByPredicate(Graph.Nodes, [&](const auto& Item) { return Item->GetFName() == Name; });
        if (!Found)
            UE_LOG(LogTemp, Error, TEXT("TrioCombat missing graph node %s in %s"), *Name.ToString(), *Graph.GetPathName());
        return Found ? Found->Get() : nullptr;
    }

    template <typename T>
    T* AddNode(UEdGraph& Graph, const UEdGraphNode& Near)
    {
        T* New = NewObject<T>(&Graph);
        Graph.AddNode(New, false, false);
        New->CreateNewGuid();
        New->NodePosX = Near.NodePosX;
        New->NodePosY = Near.NodePosY + 170;
        New->PostPlacedNewNode();
        return New;
    }

    UK2Node_CallFunction* AddCall(UEdGraph& Graph, const UEdGraphNode& Near, FName Name)
    {
        UFunction* Function = UTrioCombatLibrary::StaticClass()->FindFunctionByName(Name);
        check(Function);
        UK2Node_CallFunction* Call = AddNode<UK2Node_CallFunction>(Graph, Near);
        Call->SetFromFunction(Function);
        Call->AllocateDefaultPins();
        return Call;
    }

    bool Connect(UEdGraphPin* From, UEdGraphPin* To)
    {
        return From && To && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(From, To);
    }

    bool Transfer(UEdGraphPin* From, UEdGraphPin* To)
    {
        if (!From || !To)
            return false;
        const TArray<UEdGraphPin*> Links = From->LinkedTo;
        From->BreakAllPinLinks();
        for (UEdGraphPin* Link : Links)
            if (!Connect(Link, To))
                return false;
        return true;
    }

    UEdGraphPin* Self(UEdGraph& Graph, const UEdGraphNode& Near)
    {
        auto* Owner = AddNode<UK2Node_Self>(Graph, Near);
        Owner->AllocateDefaultPins();
        return Owner->FindPin(UEdGraphSchema_K2::PN_Self);
    }

    bool ReplaceImpact(UEdGraph& Graph, FName NodeName, FName Function, FName BreakHit = NAME_None, FName Attack = NAME_None)
    {
        auto* Old = Cast<UK2Node_CallFunction>(Node(Graph, NodeName));
        if (!Old || !Old->GetTargetFunction() || !Old->GetTargetFunction()->GetName().StartsWith(TEXT("Apply")))
            return false;
        auto* New = AddCall(Graph, *Old, Function);
        New->NodePosY = Old->NodePosY;
        if (!Attack.IsNone())
            New->FindPinChecked(TEXT("Attack"))->DefaultValue = Attack.ToString();

        UEdGraphPin* Hit = Old->FindPin(TEXT("HitInfo"));
        if (!BreakHit.IsNone())
        {
            UEdGraphNode* Break = Node(Graph, BreakHit);
            Hit = Break ? Break->FindPin(TEXT("Hit")) : nullptr;
        }

        if (!Hit || Hit->LinkedTo.Num() != 1 || !Connect(Hit->LinkedTo[0], New->FindPin(TEXT("HitInfo"))) ||
            !Transfer(Old->FindPin(TEXT("DamageCauser")), New->FindPin(TEXT("DamageCauser"))) ||
            !Transfer(Old->GetExecPin(), New->GetExecPin()) ||
            !Transfer(Old->GetThenPin(), New->GetThenPin()))
            return false;
        Old->DestroyNode();
        return true;
    }

    bool InsertBefore(UEdGraph& Graph, FName Destination, FName Function)
    {
        auto* Old = Node(Graph, Destination);
        if (!Old)
            return false;
        auto* New = AddCall(Graph, *Old, Function);
        auto* Exec = Old->FindPin(UEdGraphSchema_K2::PN_Execute);
        return Transfer(Exec, New->GetExecPin()) &&
            Connect(New->GetThenPin(), Exec) &&
            Connect(Self(Graph, *New), New->FindPin(TEXT("Attacker")));
    }

    bool InstallGuns(UBlueprint& Blueprint)
    {
        auto* GunGraph = Graph(Blueprint, TEXT("Player1FireHit"));
        return GunGraph && ReplaceImpact(*GunGraph, TEXT("K2Node_CallFunction_17"), TEXT("ResolveGunHit"));
    }

    bool InstallSniper(UBlueprint& Blueprint)
    {
        auto* Event = Graph(Blueprint, TEXT("EventGraph"));
        if (!Event || !ReplaceImpact(*Event, TEXT("K2Node_CallFunction_192"), TEXT("ResolveSniperHit")))
            return false;
        for (const TCHAR* SetPhase : {TEXT("K2Node_VariableSet_58"), TEXT("K2Node_VariableSet_60"),
            TEXT("K2Node_VariableSet_71"), TEXT("K2Node_VariableSet_74")})
            if (!InsertBefore(*Event, SetPhase, TEXT("LockSniperCharge")))
                return false;
        return true;
    }

    bool InstallBlade(UBlueprint& Blueprint)
    {
        auto* Event = Graph(Blueprint, TEXT("EventGraph"));
        if (!Event)
            return false;
        const TCHAR* Moves[][3] =
        {
            {TEXT("K2Node_CallFunction_77"), TEXT("K2Node_CallFunction_76"), TEXT("BladeFirst")},
            {TEXT("K2Node_CallFunction_104"), TEXT("K2Node_CallFunction_103"), TEXT("BladeSecond")},
            {TEXT("K2Node_CallFunction_131"), TEXT("K2Node_CallFunction_130"), TEXT("BladeThird")},
            {TEXT("K2Node_CallFunction_174"), TEXT("K2Node_CallFunction_173"), TEXT("None")},
            {TEXT("K2Node_CallFunction_201"), TEXT("K2Node_CallFunction_200"), TEXT("BladeFinish")},
            {TEXT("K2Node_CallFunction_228"), TEXT("K2Node_CallFunction_227"), TEXT("BladeRift")}
        };
        for (const auto& Move : Moves)
            if (!ReplaceImpact(*Event, Move[0], TEXT("ResolveBladeHit"), Move[1], Move[2]))
                return false;
        return InsertBefore(*Event, TEXT("K2Node_VariableSet_109"), TEXT("StartBladeAfterimage"));
    }

    UBlueprint* LoadBlueprint(const TCHAR* Path)
    {
        return LoadObject<UBlueprint>(nullptr, Path);
    }
}

bool UTrioCombatAuthoring::InstallCombatAssets()
{
    if (!MakeTable(TEXT("DT_Combatants"), FTrioCombatantRow::StaticStruct(), TEXT("Combatants.csv")) ||
        !MakeTable(TEXT("DT_Attacks"), FTrioAttackRow::StaticStruct(), TEXT("Attacks.csv")))
        return false;

    UBlueprint* Guns = LoadBlueprint(TEXT("/Game/baseAdvRootMotionSys_BP/BP_MyCharacter.BP_MyCharacter"));
    UBlueprint* Blade = LoadBlueprint(TEXT("/Game/character/Miyabi/Player/BP_Miyabi.BP_Miyabi"));
    UBlueprint* Sniper = LoadBlueprint(TEXT("/Game/character/yousun/Movement/BP_Yousun.BP_Yousun"));
    if (!Guns || !Blade || !Sniper)
        return false;

    if (!InstallGuns(*Guns) || !InstallBlade(*Blade) || !InstallSniper(*Sniper))
    {
        UE_LOG(LogTemp, Error, TEXT("TrioCombat could not wire the expected gameplay pins. Character assets were not saved."));
        return false;
    }
    for (UBlueprint* Blueprint : {Guns, Blade, Sniper})
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        if (Blueprint->Status == BS_Error)
        {
            UE_LOG(LogTemp, Error, TEXT("TrioCombat Blueprint compilation failed: %s"), *Blueprint->GetPathName());
            return false;
        }
    }
    for (UBlueprint* Blueprint : {Guns, Blade, Sniper})
        if (!SaveAsset(*Blueprint))
            return false;
    UE_LOG(LogTemp, Display, TEXT("TrioCombat installed and saved all three character Blueprints."));
    return true;
}

bool UTrioCombatAuthoring::InstallBotAttackHooks()
{
    UBlueprint* Blade = LoadBlueprint(TEXT("/Game/character/Miyabi/Player/BP_Miyabi.BP_Miyabi"));
    UBlueprint* Guns = LoadBlueprint(TEXT("/Game/baseAdvRootMotionSys_BP/BP_MyCharacter.BP_MyCharacter"));
    auto* EventGraph = Blade ? Graph(*Blade, TEXT("EventGraph")) : nullptr;
    auto* Crosshair = Guns ? Graph(*Guns, TEXT("UpdatePlayer1Crosshair")) : nullptr;
    if (!EventGraph || !Crosshair)
        return false;

    const FName AttackEvent(TEXT("TrioBotBladeAttack"));
    const bool bAlreadyInstalled = EventGraph->Nodes.ContainsByPredicate([&](const UEdGraphNode* Node)
    {
        const auto* Event = Cast<UK2Node_CustomEvent>(Node);
        return Event && Event->CustomFunctionName == AttackEvent;
    });
    if (!bAlreadyInstalled)
    {
        UEdGraphNode* PressGate = Node(*EventGraph, TEXT("K2Node_IfThenElse_4"));
        if (!PressGate)
            return false;
        auto* Event = AddNode<UK2Node_CustomEvent>(*EventGraph, *PressGate);
        Event->CustomFunctionName = AttackEvent;
        Event->NodePosX -= 360;
        Event->AllocateDefaultPins();
        if (!Connect(Event->FindPin(UEdGraphSchema_K2::PN_Then), PressGate->FindPin(UEdGraphSchema_K2::PN_Execute)))
            return false;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blade);
        FKismetEditorUtilities::CompileBlueprint(Blade);
        if (Blade->Status == BS_Error || !SaveAsset(*Blade))
            return false;
    }

    // AI controllers are local on the authority. Only a player should own the reticle.
    for (UEdGraphNode* Raw : Crosshair->Nodes)
    {
        auto* Old = Cast<UK2Node_CallFunction>(Raw);
        const auto* Function = Old ? Old->GetTargetFunction() : nullptr;
        if (!Function || Function->GetFName() != TEXT("IsLocallyControlled"))
            continue;
        auto* PlayerCheck = AddNode<UK2Node_CallFunction>(*Crosshair, *Old);
        PlayerCheck->SetFromFunction(APawn::StaticClass()->FindFunctionByName(TEXT("IsPlayerControlled")));
        PlayerCheck->NodePosY = Old->NodePosY;
        PlayerCheck->AllocateDefaultPins();
        if (!Transfer(Old->FindPin(UEdGraphSchema_K2::PN_ReturnValue), PlayerCheck->FindPin(UEdGraphSchema_K2::PN_ReturnValue)))
            return false;
        Old->DestroyNode();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Guns);
        FKismetEditorUtilities::CompileBlueprint(Guns);
        return Guns->Status != BS_Error && SaveAsset(*Guns);
    }
    return true;
}

bool UTrioCombatAuthoring::RemoveCharacterCrouch()
{
    const TCHAR* CharacterAssets[] = {
        TEXT("/Game/baseAdvRootMotionSys_BP/BP_MyCharacter.BP_MyCharacter"),
        TEXT("/Game/character/Miyabi/Player/BP_Miyabi.BP_Miyabi"),
        TEXT("/Game/character/yousun/Movement/BP_Yousun.BP_Yousun"),
        TEXT("/Game/move2/RootMotion/BP_Move2Character.BP_Move2Character"),
        TEXT("/Game/move2/Standalone/BP_Move2Character.BP_Move2Character"),
    };
    const FName CrouchFlag(TEXT("Crouch?"));
    const FName CrouchDepth(TEXT("YS_CrouchDepth"));

    for (const TCHAR* Path : CharacterAssets)
    {
        UBlueprint* Blueprint = LoadBlueprint(Path);
        if (!Blueprint)
            return false;

        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        int32 Removed = 0;
        for (UEdGraph* Graph : Graphs)
        {
            TArray<UEdGraphNode*> CrouchNodes;
            for (UEdGraphNode* Item : Graph->Nodes)
            {
                const auto* Setter = Cast<UK2Node_VariableSet>(Item);
                const auto* Call = Cast<UK2Node_CallFunction>(Item);
                const auto* Function = Call ? Call->GetTargetFunction() : nullptr;
                if ((Setter && (Setter->GetVarName() == CrouchFlag || Setter->GetVarName() == CrouchDepth)) ||
                    (Function && (Function->GetFName() == TEXT("Crouch") || Function->GetFName() == TEXT("UnCrouch"))))
                    CrouchNodes.Add(Item);
            }

            TArray<UEdGraphNode*> CrouchKeys;
            for (UEdGraphNode* Item : Graph->Nodes)
            {
                const auto* Key = Cast<UK2Node_InputKey>(Item);
                if (!Key || (Key->InputKey.GetFName() != TEXT("LeftControl") &&
                    Key->InputKey.GetFName() != TEXT("RightControl")))
                    continue;
                bool bCrouchOnly = true;
                for (const UEdGraphPin* Pin : Key->Pins)
                    for (const UEdGraphPin* Linked : Pin->LinkedTo)
                        bCrouchOnly &= CrouchNodes.Contains(Linked->GetOwningNode());
                if (bCrouchOnly)
                    CrouchKeys.Add(Item);
            }
            for (UEdGraphNode* Key : CrouchKeys)
            {
                Key->DestroyNode();
                ++Removed;
            }

            for (UEdGraphNode* Item : CrouchNodes)
            {
                UEdGraphPin* Input = Item->FindPin(UEdGraphSchema_K2::PN_Execute);
                UEdGraphPin* Output = Item->FindPin(UEdGraphSchema_K2::PN_Then);
                const TArray<UEdGraphPin*> Incoming = Input ? Input->LinkedTo : TArray<UEdGraphPin*>();
                const TArray<UEdGraphPin*> Outgoing = Output ? Output->LinkedTo : TArray<UEdGraphPin*>();
                Item->DestroyNode();
                for (UEdGraphPin* From : Incoming)
                    for (UEdGraphPin* To : Outgoing)
                        if (!Connect(From, To))
                            return false;
                ++Removed;
            }
        }

        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        auto* Defaults = Blueprint->GeneratedClass ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr;
        if (Blueprint->Status == BS_Error || !Defaults)
            return false;
        auto* Movement = Defaults->GetCharacterMovement();
        Movement->Modify();
        Movement->GetNavAgentPropertiesRef().bCanCrouch = false;
        if (const auto* Flag = FindFProperty<FBoolProperty>(Blueprint->GeneratedClass, CrouchFlag))
            Flag->SetPropertyValue_InContainer(Defaults, false);
        if (const auto* Depth = FindFProperty<FDoubleProperty>(Blueprint->GeneratedClass, CrouchDepth))
            Depth->SetPropertyValue_InContainer(Defaults, 0.0);
        if (const auto* Depth = FindFProperty<FFloatProperty>(Blueprint->GeneratedClass, CrouchDepth))
            Depth->SetPropertyValue_InContainer(Defaults, 0.0f);
        Blueprint->MarkPackageDirty();
        if (!SaveAsset(*Blueprint))
            return false;
        UE_LOG(LogTemp, Display, TEXT("TrioCombat removed %d crouch nodes from %s"), Removed, Path);
    }
    return true;
}

bool UTrioCombatAuthoring::VerifyCombatData()
{
    const auto* Combatants = LoadObject<UDataTable>(nullptr, TEXT("/TrioCombat/DT_Combatants.DT_Combatants"));
    const auto* Attacks = LoadObject<UDataTable>(nullptr, TEXT("/TrioCombat/DT_Attacks.DT_Attacks"));
    if (!Combatants || !Attacks)
        return false;
    struct FCase
    {
        const TCHAR* Move;
        double Charge;
        double Expected[4];
    };
    const FCase Cases[] = {
        {TEXT("GunSingle"), 0, {48, 72, 72, 108}},
        {TEXT("GunSweep"), 0, {22.4, 33.6, 33.6, 50.4}},
        {TEXT("BladeFirst"), 0, {40, 52, 40, 52}},
        {TEXT("BladeSecond"), 0, {40, 52, 40, 52}},
        {TEXT("BladeThird"), 0, {60, 78, 60, 78}},
        {TEXT("BladeAfterimage"), 0, {20, 26, 20, 26}},
        {TEXT("BladeRift"), 0, {17, 22.1, 17, 22.1}},
        {TEXT("BladeFinish"), 0, {80, 104, 80, 104}},
        {TEXT("Sniper"), 0, {180, 198, 270, 297}},
        {TEXT("Sniper"), 0.5, {360, 396, 540, 594}},
        {TEXT("Sniper"), 1, {540, 594, 810, 891}}
    };
    for (const FCase& Case : Cases)
    {
        const auto* Move = Attacks->FindRow<FTrioAttackRow>(Case.Move, TEXT("Verify"));
        const auto* Stats = Move ? Combatants->FindRow<FTrioCombatantRow>(Move->Character, TEXT("Verify")) : nullptr;
        if (!Stats)
            return false;
        for (int32 Flags = 0; Flags < 4; ++Flags)
        {
            const FTrioHitResult Result = UTrioCombatLibrary::Calculate(*Stats, *Move, (Flags & 2) != 0,
                Case.Charge, (Flags & 1) ? 0.0 : 1.0);
            if (!FMath::IsNearlyEqual(Result.Damage, Case.Expected[Flags]))
                return false;
        }
    }
    for (FName Id : {FName(TEXT("Guns")), FName(TEXT("Miyabi")), FName(TEXT("Yousun"))})
    {
        const auto* Stats = Combatants->FindRow<FTrioCombatantRow>(Id, TEXT("Verify"));
        if (!Stats)
            return false;
        FTrioVitals Vitals;
        Vitals.Health = Stats->MaxHealth - 10;
        Vitals.Shield = Stats->MaxShield - 60;
        Vitals.RestoreShield(*Stats, 1);
        if (Vitals.Health != Stats->MaxHealth - 10 || Vitals.Shield != Stats->MaxShield - 60 + Stats->ShieldPerSecond)
            return false;
        Vitals.RestoreShield(*Stats, 100);
        if (Vitals.Shield != Stats->MaxShield)
            return false;
        Vitals.Health = 0;
        Vitals.Shield = 0;
        Vitals.RestoreShield(*Stats, 1);
        if (Vitals.Shield != 0)
            return false;
    }
    UE_LOG(LogTemp, Display, TEXT("TrioCombat verified 44 deterministic damage outcomes and all three shield rates."));
    return true;
}

namespace
{
    bool BuildMiyabiRagdoll(UPhysicsAsset& Asset, USkeletalMesh& Mesh, const FPhysAssetCreateParams& Params)
    {
        const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
        TArray<FTransform> Pose = Skeleton.GetRefBonePose();
        for (int32 Index = 0; Index < Pose.Num(); ++Index)
        {
            const int32 Parent = Skeleton.GetParentIndex(Index);
            if (Parent != INDEX_NONE)
                Pose[Index] *= Pose[Parent];
        }
        Asset.PreviewSkeletalMesh = &Mesh;

        const struct { const TCHAR* Name; const TCHAR* Tip; float Radius; } Bodies[] =
        {
            { TEXT("Bip001-Pelvis"), TEXT("Bip001-Spine"), 12 },
            { TEXT("Bip001-Spine1"), TEXT("Bip001-Spine2"), 13 },
            { TEXT("Bip001-Spine2"), TEXT("Bip001-Neck"), 15 },
            { TEXT("Bip001-Head"), nullptr, 10 },
            { TEXT("Bip001-L-UpperArm"), TEXT("Bip001-L-Forearm"), 6 },
            { TEXT("Bip001-L-Forearm"), TEXT("Bip001-L-Hand"), 5 },
            { TEXT("Bip001-R-UpperArm"), TEXT("Bip001-R-Forearm"), 6 },
            { TEXT("Bip001-R-Forearm"), TEXT("Bip001-R-Hand"), 5 },
            { TEXT("Bip001-L-Thigh"), TEXT("Bip001-L-Calf"), 8 },
            { TEXT("Bip001-L-Calf"), TEXT("Bip001-L-Foot"), 6 },
            { TEXT("Bip001-L-Foot"), TEXT("Bip001-L-Toe0"), 6 },
            { TEXT("Bip001-R-Thigh"), TEXT("Bip001-R-Calf"), 8 },
            { TEXT("Bip001-R-Calf"), TEXT("Bip001-R-Foot"), 6 },
            { TEXT("Bip001-R-Foot"), TEXT("Bip001-R-Toe0"), 6 }
        };

        for (const auto& Body : Bodies)
        {
            const FName Name(Body.Name);
            const int32 Index = Skeleton.FindBoneIndex(Name);
            const int32 Tip = Body.Tip ? Skeleton.FindBoneIndex(FName(Body.Tip)) : INDEX_NONE;
            if (Index == INDEX_NONE || (Body.Tip && Tip == INDEX_NONE))
                return false;
            const int32 BodyIndex = FPhysicsAssetUtils::CreateNewBody(&Asset, Name, Params);
            USkeletalBodySetup* Setup = Asset.SkeletalBodySetups[BodyIndex];
            const FTransform& Bone = Pose[Index];
            const FVector LocalTip = Tip != INDEX_NONE ? Bone.InverseTransformPosition(Pose[Tip].GetLocation())
                : Bone.InverseTransformVector(FVector(0, 0, 14));
            const float Radius = Body.Radius / FMath::Max(0.01, Bone.GetScale3D().GetAbsMax());
            FKSphylElem Capsule(Radius, FMath::Max(0.0, LocalTip.Size() - Radius));
            Capsule.Center = LocalTip * 0.44;
            Capsule.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, LocalTip.GetSafeNormal()).Rotator();
            if (Body.Tip)
                Setup->AggGeom.SphylElems.Add(Capsule);
            else
                Setup->AggGeom.SphereElems.Add(FKSphereElem(Radius * 1.5f));
        }

        for (int32 Index = 0; Index < Asset.SkeletalBodySetups.Num(); ++Index)
        {
            const FName Name = Asset.SkeletalBodySetups[Index]->BoneName;
            int32 Parent = Skeleton.GetParentIndex(Skeleton.FindBoneIndex(Name));
            while (Parent != INDEX_NONE && Asset.FindBodyIndex(Skeleton.GetBoneName(Parent)) == INDEX_NONE)
                Parent = Skeleton.GetParentIndex(Parent);
            if (Parent == INDEX_NONE)
                continue;

            const FName ParentName = Skeleton.GetBoneName(Parent);
            UPhysicsConstraintTemplate* Constraint = Asset.ConstraintSetup[FPhysicsAssetUtils::CreateNewConstraint(&Asset, Name)];
            FConstraintInstance& Joint = Constraint->DefaultInstance;
            Joint.ConstraintBone1 = Name;
            Joint.ConstraintBone2 = ParentName;
            Joint.SetAngularSwing1Limit(ACM_Limited, 45.0f);
            Joint.SetAngularSwing2Limit(ACM_Limited, 40.0f);
            Joint.SetAngularTwistLimit(ACM_Limited, 35.0f);
            Joint.SnapTransformsToDefault(EConstraintTransformComponentFlags::All, &Asset);
            Constraint->SetDefaultProfile(Joint);
        }

        // This character has auxiliary twist, hair and outfit bones. Self-contact is disabled
        // for the simplified capsules so a folded pose cannot kick the body off the floor.
        for (int32 Body = 0; Body < Asset.SkeletalBodySetups.Num(); ++Body)
            for (int32 Other = Body + 1; Other < Asset.SkeletalBodySetups.Num(); ++Other)
                Asset.DisableCollision(Body, Other);

        Asset.UpdateBodySetupIndexMap();
        Asset.UpdateBoundsBodiesArray();
        return true;
    }
}

bool UTrioCombatAuthoring::BuildDeathPhysics()
{
    const TCHAR* Meshes[] =
    {
        TEXT("/Game/character/double_guns/SK_DoubleGuns_Cloth.SK_DoubleGuns_Cloth"),
        TEXT("/Game/character/Vio_Character5/SK_Vio_Role2Shoes.SK_Vio_Role2Shoes"),
        TEXT("/Game/character/yousun/TriggerBinding/SK_Yousun_HairBound.SK_Yousun_HairBound")
    };
    for (const TCHAR* Path : Meshes)
    {
        auto* Mesh = LoadObject<USkeletalMesh>(nullptr, Path);
        if (!Mesh)
            return false;
        const FString Name = TEXT("PA_Death_") + Mesh->GetName();
        const FString PackageName = TEXT("/TrioCombat/Death/") + Name;
        if (auto* Existing = LoadObject<UPhysicsAsset>(nullptr, *(PackageName + TEXT(".") + Name), nullptr, LOAD_NoWarn))
        {
            if (Mesh->GetFName() == TEXT("SK_Vio_Role2Shoes"))
            {
                const int32 HeadIndex = Existing->FindBodyIndex(TEXT("Bip001-Head"));
                USkeletalBodySetup* Head = Existing->SkeletalBodySetups.IsValidIndex(HeadIndex)
                    ? Existing->SkeletalBodySetups[HeadIndex].Get() : nullptr;
                if (Head && Head->AggGeom.SphylElems.Num() == 1)
                {
                    const float Radius = Head->AggGeom.SphylElems[0].Radius;
                    Head->AggGeom.SphylElems.Reset();
                    Head->AggGeom.SphereElems.Add(FKSphereElem(Radius * 1.5f));
                    Head->InvalidatePhysicsData();
                    Existing->MarkPackageDirty();
                    if (!SaveAsset(*Existing))
                        return false;
                }
            }
            continue;
        }

        auto* Asset = NewObject<UPhysicsAsset>(CreatePackage(*PackageName), *Name, RF_Public | RF_Standalone | RF_Transactional);
        FPhysAssetCreateParams Params;
        Params.MinBoneSize = 9.0f;
        FText Error;
        const bool bBuilt = Mesh->GetFName() == TEXT("SK_Vio_Role2Shoes")
            ? BuildMiyabiRagdoll(*Asset, *Mesh, Params)
            : FPhysicsAssetUtils::CreateFromSkeletalMesh(Asset, Mesh, Params, Error, false, false);
        if (!bBuilt || Asset->SkeletalBodySetups.Num() < 5 || Asset->ConstraintSetup.Num() < 4)
        {
            UE_LOG(LogTemp, Error, TEXT("Ragdoll generation failed for %s (%d bodies, %d constraints): %s"),
                *Mesh->GetName(), Asset->SkeletalBodySetups.Num(), Asset->ConstraintSetup.Num(), *Error.ToString());
            return false;
        }

        for (USkeletalBodySetup* Body : Asset->SkeletalBodySetups)
        {
            Body->DefaultInstance.LinearDamping = 0.65f;
            Body->DefaultInstance.AngularDamping = 2.3f;
        }
        FAssetRegistryModule::AssetCreated(Asset);
        Asset->MarkPackageDirty();
        if (!SaveAsset(*Asset))
            return false;
        UE_LOG(LogTemp, Display, TEXT("%s: %d bodies and %d constraints"), *Name,
            Asset->SkeletalBodySetups.Num(), Asset->ConstraintSetup.Num());
    }
    return true;
}

