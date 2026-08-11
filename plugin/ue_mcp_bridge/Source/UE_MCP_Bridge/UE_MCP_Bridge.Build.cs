using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using UnrealBuildTool;

public class UE_MCP_Bridge : ModuleRules
{
	public UE_MCP_Bridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		GenerateAtomicBridgeBuildMetadata();

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
				"JsonUtilities",
				"Projects",
				"GameplayTags",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AIModule",
				"MessageLog",
				"AnimGraph",
				"AnimationEditor",
				"AnimationModifiers",
				"AssetRegistry",
				"AssetTools",
				"AudioEditor",
				"AudioMixer",
				"AudioExtensions",
				"MetasoundEngine",
				"MetasoundFrontend",
				"MetasoundGraphCore",
				"Synthesis",
				"BSPUtils",
				"BlueprintEditorLibrary",
				"BlueprintGraph",
				"Blutility",
				"Chooser",
				"ContentBrowser",
				"ControlRig",
				"ControlRigDeveloper",
				"RigVMDeveloper",
				"DataValidation",
				"EditorScriptingUtilities",
				"EditorStyle",
				"EditorSubsystem",
				"EditorWidgets",
				"EnhancedInput",
				"Foliage",
				"GameProjectGeneration",
				"GameplayAbilities",
				"GameplayTasks",
				"HTTP",
				"IKRig",
				"IKRigDeveloper",
				"IKRigEditor",
				"ImageWrapper",
				"InputCore",
				"Kismet",
				"KismetCompiler",
				"Landscape",
				"LevelEditor",
				"LevelSequence",
				"LevelSequenceEditor",
				"MaterialEditor",
				"MovieScene",
				"MovieSceneTracks",
				"MeshDescription",
				"NavigationSystem",
				"Niagara",
				"NiagaraEditor",
				"PCG",
				"PCGEditor",
				"PoseSearch",
				"PoseSearchEditor",
				"PlatformCrypto",
				"PlatformCryptoContext",
				"PropertyBindingUtils",
				"PropertyEditor",
				"PythonScriptPlugin",
				"Sequencer",
				"Settings",
				"Slate",
				"SlateCore",
				"StateTreeModule",
				"StateTreeEditorModule",
				"StaticMeshDescription",
				"ClothingSystemRuntimeCommon",
				"ClothingSystemRuntimeInterface",
				"StructUtils",
				"SubobjectDataInterface",
				"ToolMenus",
				"RenderCore",
				"RHI",
				"UMG",
				"UMGEditor",
				"UnrealEd",
				"WebSockets",
				"WorkspaceMenuStructure",
			}
		);

		// LiveCoding is Windows-only (Developer/Windows/LiveCoding)
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}

		// Fab is Epic's marketplace plugin. It ships enabled by default on UE 5.8
		// but is absent on older engines and can be disabled, so we do not hard
		// depend on it: detect the plugin on disk and only then link its native
		// import/cache API, guarding those code paths with WITH_FAB_PLUGIN. When
		// absent, the Fab handlers still register and fall back to console-command
		// paths (login/sync/clear) or return a clean "not available" error.
		bool bFabPluginPresent = System.IO.Directory.Exists(
			System.IO.Path.Combine(EngineDirectory, "Plugins", "Fab"));
		if (bFabPluginPresent && Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("Fab");
			PublicDefinitions.Add("WITH_FAB_PLUGIN=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_FAB_PLUGIN=0");
		}
	}

	private void GenerateAtomicBridgeBuildMetadata()
	{
		string pluginRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		string[] fingerprintInputs = new string[]
		{
			"Contracts/AtomicBlueprint/receipt-v1.schema.json",
			"Contracts/AtomicBlueprint/request-v1.schema.json",
			"Source/UE_MCP_Bridge/Private/Handlers/BlueprintTopologySerializer.cpp",
			"Source/UE_MCP_Bridge/Private/Handlers/BlueprintTopologySerializer.h",
			"Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers.cpp",
			"Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers.h",
			"Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp",
			"Source/UE_MCP_Bridge/UE_MCP_Bridge.Build.cs",
		};

		using MemoryStream framedSource = new MemoryStream();
		using (BinaryWriter writer = new BinaryWriter(framedSource, new UTF8Encoding(false), true))
		{
			foreach (string relativePath in fingerprintInputs.OrderBy(value => value, StringComparer.Ordinal))
			{
				string absolutePath = Path.Combine(pluginRoot, relativePath.Replace('/', Path.DirectorySeparatorChar));
				if (!File.Exists(absolutePath))
				{
					throw new BuildException("Atomic bridge fingerprint input is missing: {0}", absolutePath);
				}
				byte[] relativeBytes = Encoding.UTF8.GetBytes(relativePath);
				byte[] sourceBytes = File.ReadAllBytes(absolutePath);
				writer.Write(relativeBytes.Length);
				writer.Write(relativeBytes);
				writer.Write(sourceBytes.Length);
				writer.Write(sourceBytes);
			}
		}

		string fingerprint;
		using (SHA256 sha256 = SHA256.Create())
		{
			fingerprint = String.Concat(sha256.ComputeHash(framedSource.ToArray()).Select(value => value.ToString("x2")));
		}
		string gitCommit = Environment.GetEnvironmentVariable("SPACEHEAD_BRIDGE_GIT_COMMIT") ?? String.Empty;
		if (!Regex.IsMatch(gitCommit, "^[0-9a-fA-F]{40}$")) gitCommit = String.Empty;
		gitCommit = gitCommit.ToLowerInvariant();

		string generatedDirectory = Path.Combine(pluginRoot, "Intermediate", "Generated", "AtomicBridgeBuildIdentity");
		Directory.CreateDirectory(generatedDirectory);
		string generatedHeader = Path.Combine(generatedDirectory, "AtomicBridgeBuildIdentity.generated.h");
		string contents =
			"#pragma once\n\n" +
			"namespace UE_MCP_AtomicBuildIdentity\n{\n" +
			"\tstatic constexpr const TCHAR* GitCommit = TEXT(\"" + gitCommit + "\");\n" +
			"\tstatic constexpr const TCHAR* BuildFingerprint = TEXT(\"" + fingerprint + "\");\n" +
			"\tstatic constexpr const TCHAR* PluginBuildIdentity = TEXT(\"sha256:" + fingerprint + "\");\n" +
			"}\n";
		if (!File.Exists(generatedHeader) || File.ReadAllText(generatedHeader) != contents)
		{
			File.WriteAllText(generatedHeader, contents, new UTF8Encoding(false));
		}
		PrivateIncludePaths.Add(generatedDirectory);
	}
}
