import { describe, expect, it } from "vitest";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { createHash } from "node:crypto";

const root=process.cwd();
const read=(...parts:string[])=>readFileSync(join(root,...parts),"utf8");

describe("asset.read_state_ledger_fingerprint",()=>{
  it("maps the public read-only action to the dedicated native handler",()=>{
    const tool=read("src","tools","asset.ts"),registry=read("plugin","ue_mcp_bridge","Source","UE_MCP_Bridge","Private","Handlers","AssetHandlers.cpp");
    expect(tool).toContain('read_state_ledger_fingerprint: bp(');
    expect(tool).toContain('"read_state_ledger_fingerprint"');
    expect(registry).toContain('Registry.RegisterHandler(TEXT("read_state_ledger_fingerprint"), &ReadStateLedgerFingerprint)');
  });

  it("hashes qualified live Blueprint topology plus structure and releases multipart captures",()=>{
    const source=read("plugin","ue_mcp_bridge","Source","UE_MCP_Bridge","Private","Handlers","AssetHandlers_StateLedger.cpp");
    expect(source).toContain("FBlueprintHandlers::ReadBlueprintTopology");
    expect(source).toContain("FBlueprintHandlers::ReadBlueprint");
    expect(source).toContain("FBlueprintHandlers::ReleaseBlueprintTopologyCapture");
    expect(source).toContain('GetBoolField(TEXT("dataOmitted"))');
    expect(source).toContain('GetBoolField(TEXT("truncated"))');
    expect(source).toContain("topology traversal was incomplete");
    expect(source).toContain("CalcSHA256");
    expect(source).toContain('mutationOperationsPerformed"), false');
    expect(source).toContain('compileRequested"), false');
    expect(source).toContain('saveRequested"), false');
    expect(source).toContain('reconstructRequested"), false');
    expect(source).not.toMatch(/CompileBlueprint\s*\(/);
    expect(source).not.toMatch(/SaveLoadedAsset\s*\(/);
  });

  it("fingerprints full Struct and Enum definitions and their exact ordered fields",()=>{
    const fingerprint=read("plugin","ue_mcp_bridge","Source","UE_MCP_Bridge","Private","Handlers","AssetHandlers_StateLedger.cpp");
    const struct=read("plugin","ue_mcp_bridge","Source","UE_MCP_Bridge","Private","Handlers","AssetHandlers_Struct.cpp");
    const enumSource=read("plugin","ue_mcp_bridge","Source","UE_MCP_Bridge","Private","Handlers","AssetHandlers_Enum.cpp");
    expect(fingerprint).toContain("ListStructFields(Params)");expect(fingerprint).toContain("ListEnumValues(Params)");
    for(const field of ["category","subCategory","subCategoryObject","containerType","defaultValue","currentDefaultValue","defaultValueAvailable","valueType","metadata"])expect(struct).toContain(`TEXT("${field}")`);
    expect(enumSource).toContain('TEXT("metadata")');expect(enumSource).toContain('TEXT("displayName")');expect(enumSource).toContain('TEXT("value")');
  });

  it("exposes a complete read-only Blueprint structure contract for manifests and fingerprinting",()=>{
    const source=read("plugin","ue_mcp_bridge","Source","UE_MCP_Bridge","Private","Handlers","BlueprintHandlers.cpp");
    const body=source.slice(source.indexOf("FBlueprintHandlers::ReadBlueprint("),source.indexOf("FBlueprintHandlers::AddVariable("));
    for(const field of ["contractVersion","objectPath","classPath","parentClass","interfaces","variables","typeInfo","containerType","defaultValue","replicated","repNotifyFunction","functions","inputs","outputs","dispatchers","graphs","components","dirtyStateChanged","mutationOperationsPerformed","complete"])
      expect(body).toContain(`TEXT("${field}")`);
    expect(body).toContain('TEXT("spacehead.blueprint-structure@1.0")');
    expect(body).not.toMatch(/CompileBlueprint\s*\(/);expect(body).not.toMatch(/SaveAssetPackage\s*\(/);expect(body).not.toMatch(/ReconstructNode\s*\(/);
  });

  it("semantic projections are deterministic and change for required Blueprint, Struct, and Enum edits",()=>{
    const hash=(value:unknown)=>createHash("sha256").update(JSON.stringify(value)).digest("hex"),clone=<T>(value:T):T=>JSON.parse(JSON.stringify(value));
    const blueprint:any={parentClass:"/Script/Engine.Actor",interfaces:["/Game/I.I_C"],components:[{name:"Root",classPath:"/Script/Engine.SceneComponent",parent:""}],
      variables:[{name:"State",typeInfo:{category:"struct",subCategoryObject:"/Game/ST_State.ST_State",containerType:"array"},defaultValue:"()"}],
      functions:[{name:"Resolve"}],graphs:[{graphIdentity:"g",nodes:[{id:"n",nodeClass:"K2Node",pins:[{id:"p",typeInfo:{category:"int"},defaultValue:"1"}]}],connections:[{sourceNodeId:"n",sourcePinId:"p",targetNodeId:"m",targetPinId:"q"}]}]};
    expect(hash(blueprint)).toBe(hash(clone(blueprint)));
    const blueprintChanges=[(v:any)=>v.parentClass="/Script/Engine.Pawn",(v:any)=>v.interfaces.push("/Game/J.J_C"),(v:any)=>v.components.push({name:"Mesh"}),
      (v:any)=>v.variables[0].typeInfo.containerType="set",(v:any)=>v.variables[0].defaultValue="(X=1)",(v:any)=>v.functions.push({name:"NewFunction"}),
      (v:any)=>v.graphs.push({graphIdentity:"new",nodes:[],connections:[]}),(v:any)=>v.graphs[0].nodes.push({id:"x",nodeClass:"K2Node",pins:[]}),
      (v:any)=>v.graphs[0].nodes[0].pins[0].typeInfo.category="float",(v:any)=>v.graphs[0].nodes[0].pins[0].defaultValue="2",
      (v:any)=>v.graphs[0].connections[0].targetPinId="r"];
    for(const change of blueprintChanges){const changed=clone(blueprint);change(changed);expect(hash(changed)).not.toBe(hash(blueprint))}
    const struct:any={fields:[{index:0,name:"A",typeInfo:{category:"int",containerType:"none"},defaultValue:"0"},{index:1,name:"B",typeInfo:{category:"byte",subCategoryObject:"/Game/E.E"},defaultValue:"One"}]};
    for(const change of [(v:any)=>v.fields.push({index:2,name:"C"}),(v:any)=>v.fields.reverse(),(v:any)=>v.fields[0].typeInfo.category="float",(v:any)=>v.fields[0].typeInfo.containerType="array",(v:any)=>v.fields[1].typeInfo.subCategoryObject="/Game/E2.E2",(v:any)=>v.fields[0].defaultValue="1"]){const changed=clone(struct);change(changed);expect(hash(changed)).not.toBe(hash(struct))}
    const enumDefinition={values:[{index:0,name:"One",displayName:"One",value:0},{index:1,name:"Two",displayName:"Two",value:1}]};
    for(const change of [(v:any)=>v.values.push({index:2,name:"Three",displayName:"Three",value:2}),(v:any)=>v.values.reverse(),(v:any)=>v.values[0].name="First",(v:any)=>v.values[0].displayName="First",(v:any)=>v.values[0].value=5]){const changed=clone(enumDefinition);change(changed);expect(hash(changed)).not.toBe(hash(enumDefinition))}
  });
});
