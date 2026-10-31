#include "MaterialProof.h"
#include "BuildingTarget.h"
#include "ProofReflection.h"
#include "RenderingProofPixels.h"
#include "plugin_helpers.h"
#include "plugin.h"
#include <array>
#include <cmath>

namespace
{
    bool ValidateReflection()
    {
        using namespace MainMapProofReflection;
        return Require(SDK::UKismetMaterialLibrary::StaticClass(),"KismetMaterialLibrary","CreateDynamicMaterialInstance",0x28,
            {{"WorldContextObject",0,8,"ObjectProperty"},{"Parent",8,8,"ObjectProperty"},
             {"OptionalName",16,8,"NameProperty"},{"CreationFlags",24,1,"EnumProperty"},{"ReturnValue",32,8,"ObjectProperty"}}) &&
            Require(SDK::UKismetStringLibrary::StaticClass(),"KismetStringLibrary","Conv_StringToName",0x18,
            {{"InString",0,16,"StrProperty"},{"ReturnValue",16,8,"NameProperty"}}) &&
            Require(SDK::UMaterialInstanceDynamic::StaticClass(),"MaterialInstanceDynamic","K2_GetTextureParameterValue",0x10,
            {{"ParameterName",0,8,"NameProperty"},{"ReturnValue",8,8,"ObjectProperty"}}) &&
            Require(SDK::UMaterialInstanceDynamic::StaticClass(),"MaterialInstanceDynamic","SetTextureParameterValue",0x10,
            {{"ParameterName",0,8,"NameProperty"},{"Value",8,8,"ObjectProperty"}}) &&
            Require(SDK::UKismetRenderingLibrary::StaticClass(),"KismetRenderingLibrary","ImportBufferAsTexture2D",0x20,
            {{"WorldContextObject",0,8,"ObjectProperty"},{"Buffer",8,16,"ArrayProperty",0x8000182},{"ReturnValue",24,8,"ObjectProperty"}}) &&
            MainMapProof::BuildingTarget::ValidateReflection();
    }

    std::array<SDK::FCanvasUVTri,2> Quad(double x,double y,double w,double h,int patch,SDK::FLinearColor tint,bool mirror=false)
    {
        const double u=(patch%4*16+8)/64.0, v=(patch/4*32+16)/64.0;
        // Interior subrectangle, rather than the entire source or an edge/gutter sample.
        const double du=3.0/64.0,dv=6.0/64.0;
        SDK::FVector2D p[4]={{x,y},{x+w,y},{x+w,y+h},{x,y+h}};
        SDK::FVector2D uv[4]={{u-du,v-dv},{u+du,v-dv},{u+du,v+dv},{u-du,v+dv}};
        if(mirror) { std::swap(uv[0],uv[1]); std::swap(uv[2],uv[3]); }
        std::array<SDK::FCanvasUVTri,2> result{};
        int indices[2][3]={{0,1,2},{0,2,3}};
        for(int i=0;i<2;++i)
        {
            auto& t=result[i]; const auto* k=indices[i];
            t.V0_Pos=p[k[0]];t.V1_Pos=p[k[1]];t.V2_Pos=p[k[2]];
            t.V0_UV=uv[k[0]];t.V1_UV=uv[k[1]];t.V2_UV=uv[k[2]];
            t.V0_Color=t.V1_Color=t.V2_Color=tint;
        }
        return result;
    }
}

namespace MainMapProof
{
    bool PresentationFrame::Reset()
    {
        // Withdraw the image/brush first. Keep dependencies until the MIDs
        // relinquish their references. Every failed reset remains retryable.
        if (!presentMidLease.Reset()) return false;
        presentMid=nullptr;
        if (!composeMidLease.Reset()) return false;
        composeMid=nullptr;
        if (!solidMidLease.Reset()) return false;
        solidMid=nullptr;
        if (!textureLease.Reset() || !target.Reset()) return false;
        submitted=false;
        return true;
    }

    SDK::UMaterial* LoadMaterial(const char* path, SDK::EMaterialDomain expectedDomain,
        MainMapNative::StrongObjectLease& lease)
    {
        if (!MainMapNative::CanOperate() || !g_self || !g_self->hooks ||
            !g_self->hooks->Pak || !g_self->hooks->Pak->LoadObject) return nullptr;
        auto* object=static_cast<SDK::UObject*>(g_self->hooks->Pak->LoadObject(path));
        if (!object || !lease.Acquire(object))
        {
            LOG_ERROR("MainMap A2.3: material load/retention FAILED: %s",path);
            return nullptr;
        }
        const std::string loadedPath=object->Outer ?
            object->Outer->Name.GetRawString()+"."+object->Name.GetRawString() : "";
        if (!object->IsA(SDK::UMaterial::StaticClass()) || loadedPath!=path)
        {
            LOG_ERROR("MainMap A2.3: wrong material identity path=%s actual=%s",path,loadedPath.c_str());
            return nullptr;
        }
        auto* material=static_cast<SDK::UMaterial*>(object);
        const bool accepted=material->MaterialDomain==expectedDomain &&
            material->BlendMode==SDK::EBlendMode::BLEND_AlphaComposite;
        LOG_INFO("MainMap A2.3: exact object=%s domain=%u blend=%u domain/blend=%s",path,
            static_cast<unsigned>(material->MaterialDomain),static_cast<unsigned>(material->BlendMode),
            accepted?"PASS":"FAIL");
        return accepted?material:nullptr;
    }

    static bool PrepareFrame(SDK::UWorld* world,SDK::UMaterial* compose,SDK::UMaterial* present,
        PresentationFrame& frame,bool readback)
    {
        if(!world || !compose || !present || !MainMapNative::CanOperate() || !ValidateReflection()) return false;
        auto*& composeMid=frame.composeMid;
        auto*& presentMid=frame.presentMid;
        auto*& solidMid=frame.solidMid;
        auto* mid=SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(world,compose,{},SDK::EMIDCreationFlags::None);
        if(!mid || !frame.composeMidLease.Acquire(mid)) return false;
        composeMid=mid;
        mid=SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(world,present,{},SDK::EMIDCreationFlags::None);
        if(!mid || !frame.presentMidLease.Acquire(mid)) return false;
        presentMid=mid;
        mid=SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(world,compose,{},SDK::EMIDCreationFlags::None);
        if(!mid || !frame.solidMidLease.Acquire(mid)) return false;
        solidMid=mid;
        const auto spriteName=SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"SpriteAtlas"));
        const auto targetName=SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"BuildingTarget"));
        auto* spriteDefault=composeMid->K2_GetTextureParameterValue(spriteName);
        auto* targetDefault=presentMid->K2_GetTextureParameterValue(targetName);
        const bool parameters=spriteDefault && targetDefault && spriteDefault->IsA(SDK::UTexture2D::StaticClass()) &&
            targetDefault->IsA(SDK::UTexture2D::StaticClass()) && spriteDefault->SRGB && !targetDefault->SRGB;
        LOG_INFO("MainMap A2.3: Texture2D parameters SpriteAtlas=%p sRGB=%d BuildingTarget=%p sRGB=%d %s",
            spriteDefault,spriteDefault?int(spriteDefault->SRGB):-1,targetDefault,targetDefault?int(targetDefault->SRGB):-1,
            parameters?"PASS":"FAIL");
        if(!parameters) return false;
        SDK::TArray<SDK::uint8> png(MainMapRenderingProofPng,sizeof(MainMapRenderingProofPng),sizeof(MainMapRenderingProofPng));
        auto* texture=SDK::UKismetRenderingLibrary::ImportBufferAsTexture2D(world,png);
        if(!texture || !frame.textureLease.Acquire(texture)) return false;
        if(!texture->SRGB) { LOG_ERROR("MainMap A2.3: imported texture not sRGB"); return false; }
        texture->Filter=SDK::ETextureFilter::TF_Bilinear;
        texture->AddressX=texture->AddressY=SDK::ETextureAddress::TA_Clamp;
        composeMid->SetTextureParameterValue(spriteName,texture);
        solidMid->SetTextureParameterValue(spriteName,texture);
        if(composeMid->K2_GetTextureParameterValue(spriteName)!=texture) return false;
        auto& target=frame.target;
        if(!target.Create(world,readback)) return false;
        presentMid->SetTextureParameterValue(targetName,target.Get());
        if(presentMid->K2_GetTextureParameterValue(targetName)!=target.Get()) return false;
        return true;
    }

    static bool SubmitFrame(PresentationFrame& frame,bool alternate,bool rejectAfterBegin)
    {
        auto& target=frame.target;
        auto* composeMid=frame.composeMid;
        auto* solidMid=frame.solidMid;
        if(!target.Begin()) return false;
        if(rejectAfterBegin)
        {
            target.End();
            LOG_INFO("MainMap A2.3: deliberate candidate rejection after valid Begin; exactly one End; no publication");
            return false;
        }
        const float b=128.0f/255.0f,s=b*b;
        auto draw=[&](double x,double y,int patch,SDK::FLinearColor color,bool mirror=false)
        {auto triangles=Quad(x,y,192,192,patch,color,mirror);target.Draw(composeMid,triangles.data(),2);};
        draw(64,64,6,{1,1,1,b});                         // red only
        draw(320,64,2,{0,0,1,b});                        // blue only via white sample
        draw(576,64,6,{1,1,1,b});draw(576,64,2,{0,0,1,b}); // blue after red
        draw(832,64,2,{0,0,1,b});draw(832,64,6,{1,1,1,b}); // red after blue
        draw(64,320,3,{1,1,1,1});draw(320,320,4,{1,1,1,1});draw(576,320,5,{1,1,1,1});
        draw(832,320,7,{1,1,1,1});                        // gray texture
        draw(1088,320,2,{0.5f,0.5f,0.5f,1});             // linear mid-gray vertex tint
        draw(64,576,1,{1,1,1,1});                        // transparent saturated source
        draw(320,576,6,{1,1,1,b},true);                  // mirrored fractional quad
        auto solid=Quad(576,576,192,192,2,{0,1,0,b});
        solid[0].V0_UV=solid[0].V1_UV=solid[0].V2_UV={40.0/64.0,16.0/64.0};
        target.Draw(solidMid,solid.data(),1);
        auto reverse=solid[0];reverse.V0_Pos.X+=256;reverse.V1_Pos.X+=256;reverse.V2_Pos.X+=256;
        std::swap(reverse.V0_Pos,reverse.V2_Pos);std::swap(reverse.V0_UV,reverse.V2_UV);
        target.Draw(solidMid,&reverse,1);
        auto marker=Quad(1408,64,384,384,2,alternate?SDK::FLinearColor{0,1,0,1}:SDK::FLinearColor{1,0,0,1});
        target.Draw(composeMid,marker.data(),2);
        auto transparentBlack=Quad(1088,576,192,192,0,{1,1,1,1});
        target.Draw(composeMid,transparentBlack.data(),2);
        auto skew=Quad(64,960,192,192,6,{1,1,1,b});
        skew[0].V0_Pos={64,960};skew[0].V1_Pos={256,928};skew[0].V2_Pos={288,1152};
        skew[1].V0_Pos={64,960};skew[1].V1_Pos={288,1152};skew[1].V2_Pos={96,1184};
        target.Draw(composeMid,skew.data(),2);
        target.End();
        frame.submitted=true;
        return true;
    }

    bool BuildPresentationFrame(SDK::UWorld* world,SDK::UMaterial* compose,SDK::UMaterial* present,
        PresentationFrame& frame,bool alternate,bool rejectAfterBegin)
    {
        return PrepareFrame(world,compose,present,frame,false) && SubmitFrame(frame,alternate,rejectAfterBegin);
    }

    bool ValidateAndMeasure(SDK::UWorld* world,SDK::UMaterial* compose,SDK::UMaterial* present)
    {
        PresentationFrame frame;
        if(!PrepareFrame(world,compose,present,frame,true) || !SubmitFrame(frame,false,false)) return false;
        auto& target=frame.target;
        const float b=128.0f/255.0f,s=b*b;
        bool pass=true;
        auto check=[&](const char* name,int x,int y,SDK::FLinearColor expected,float tolerance=0.005f)
        {const bool ok=target.Check(name,x,y,expected,tolerance);pass=ok&&pass;};
        check("exterior",16,16,{0,0,0,1},0.001f);
        check("red-only",160,160,{s,0,0,1-s});check("blue-only",416,160,{0,0,b,1-b});
        check("blue-after-red",672,160,{s*(1-b),0,b,(1-s)*(1-b)});
        check("red-after-blue",928,160,{s,0,b*(1-s),(1-s)*(1-b)});
        check("primary-red",160,416,{1,0,0,0});check("primary-green",416,416,{0,1,0,0});
        check("primary-blue",672,416,{0,0,1,0});
        const float gray=std::pow((b+0.055f)/1.055f,2.4f);
        check("gray-texture",928,416,{gray,gray,gray,0});
        const float tint=std::pow((188.0f/255.0f+0.055f)/1.055f,2.4f);
        check("mid-gray-vertex-decode",1184,416,{tint,tint,tint,0});
        check("transparent-saturated",160,672,{0,0,0,1},0.001f);
        check("mirrored",416,672,{s,0,0,1-s});
        check("solid-triangle",704,608,{0,b,0,1-b});
        check("reverse-winding",960,608,{0,b,0,1-b});
        check("shared-diagonal",160,160,{s,0,0,1-s});
        check("diagonal-left",159,160,{s,0,0,1-s});check("diagonal-right",161,160,{s,0,0,1-s});
        check("transparent-black",1184,672,{0,0,0,1},0.001f);
        check("skewed-quad",176,1056,{s,0,0,1-s});
        LOG_INFO("MainMap A2.3: composition shader/pixel diagnostic %s; Slate/presentation/lifetime gate still CLOSED",
            pass?"PASS":"FAIL");
        return pass;
    }
}
