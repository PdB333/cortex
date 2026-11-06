// Reference answers for Cortex's .NET metadata reader and CIL disassembler,
// produced with System.Reflection.Metadata and the runtime's OpCodes table
// (independent of Cortex's own tables). Output format = Cortex's format:
//
//   types       ilasm-style names, nested types as Outer/Inner
//   signatures  "ret (p1, p2)" for methods, the type for fields;
//               !n / !!n generic parameters, T[] T& T*, Name`n<args>
//   IL          one row per instruction: offset, opcode name, operand text,
//               token (for metadata operands)
//
//   dotnet run --project Oracle -- <assembly.dll> > expected.json
using System.Collections.Immutable;
using System.Globalization;
using System.Reflection;
using System.Reflection.Emit;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

static class Oracle
{
    static MetadataReader reader = null!;
    static readonly Dictionary<short, OpCode> opcodes = typeof(OpCodes)
        .GetFields(BindingFlags.Public | BindingFlags.Static)
        .Select(f => (OpCode)f.GetValue(null)!)
        .ToDictionary(o => o.Value);

    static string Token(EntityHandle handle) => "0x" + MetadataTokens.GetToken(handle).ToString("x8");
    static string Token(int token) => "0x" + token.ToString("x8");

    sealed class Names : ISignatureTypeProvider<string, object?>
    {
        public string GetPrimitiveType(PrimitiveTypeCode code) => code switch
        {
            PrimitiveTypeCode.Void => "void", PrimitiveTypeCode.Boolean => "bool", PrimitiveTypeCode.Char => "char",
            PrimitiveTypeCode.SByte => "int8", PrimitiveTypeCode.Byte => "uint8", PrimitiveTypeCode.Int16 => "int16",
            PrimitiveTypeCode.UInt16 => "uint16", PrimitiveTypeCode.Int32 => "int32", PrimitiveTypeCode.UInt32 => "uint32",
            PrimitiveTypeCode.Int64 => "int64", PrimitiveTypeCode.UInt64 => "uint64", PrimitiveTypeCode.Single => "float32",
            PrimitiveTypeCode.Double => "float64", PrimitiveTypeCode.String => "string", PrimitiveTypeCode.Object => "object",
            PrimitiveTypeCode.IntPtr => "native int", PrimitiveTypeCode.UIntPtr => "native uint",
            PrimitiveTypeCode.TypedReference => "typedref", _ => "?",
        };
        public string GetTypeFromDefinition(MetadataReader r, TypeDefinitionHandle h, byte kind) => TypeDefName(h);
        public string GetTypeFromReference(MetadataReader r, TypeReferenceHandle h, byte kind) => TypeRefName(h);
        public string GetTypeFromSpecification(MetadataReader r, object? c, TypeSpecificationHandle h, byte kind) =>
            r.GetTypeSpecification(h).DecodeSignature(this, c);
        public string GetSZArrayType(string e) => e + "[]";
        public string GetArrayType(string e, ArrayShape shape) => e + "[" + new string(',', shape.Rank - 1) + "]";
        public string GetByReferenceType(string e) => e + "&";
        public string GetPointerType(string e) => e + "*";
        public string GetGenericInstantiation(string g, ImmutableArray<string> args) => g + "<" + string.Join(",", args) + ">";
        public string GetGenericTypeParameter(object? c, int i) => "!" + i;
        public string GetGenericMethodParameter(object? c, int i) => "!!" + i;
        public string GetFunctionPointerType(MethodSignature<string> s) => "method";
        public string GetModifiedType(string modifier, string unmodified, bool required) => unmodified;
        public string GetPinnedType(string e) => e;
    }
    static readonly Names names = new();

    static string TypeDefName(TypeDefinitionHandle h)
    {
        var t = reader.GetTypeDefinition(h);
        var name = reader.GetString(t.Name);
        if (t.IsNested) return TypeDefName(t.GetDeclaringType()) + "/" + name;
        var ns = reader.GetString(t.Namespace);
        return ns.Length == 0 ? name : ns + "." + name;
    }

    static string TypeRefName(TypeReferenceHandle h)
    {
        var t = reader.GetTypeReference(h);
        var name = reader.GetString(t.Name);
        if (t.ResolutionScope.Kind == HandleKind.TypeReference) return TypeRefName((TypeReferenceHandle)t.ResolutionScope) + "/" + name;
        var ns = reader.GetString(t.Namespace);
        return ns.Length == 0 ? name : ns + "." + name;
    }

    static string MethodSig(MethodSignature<string> s) => s.ReturnType + " (" + string.Join(", ", s.ParameterTypes) + ")";

    static string ParentName(EntityHandle parent) => parent.Kind switch
    {
        HandleKind.TypeDefinition => TypeDefName((TypeDefinitionHandle)parent),
        HandleKind.TypeReference => TypeRefName((TypeReferenceHandle)parent),
        HandleKind.TypeSpecification => reader.GetTypeSpecification((TypeSpecificationHandle)parent).DecodeSignature(names, null),
        HandleKind.MethodDefinition => MemberName((MethodDefinitionHandle)parent),
        HandleKind.ModuleReference => "[" + reader.GetString(reader.GetModuleReference((ModuleReferenceHandle)parent).Name) + "]",
        _ => "?",
    };

    static string MemberName(EntityHandle h)
    {
        switch (h.Kind)
        {
            case HandleKind.MethodDefinition:
            {
                var m = reader.GetMethodDefinition((MethodDefinitionHandle)h);
                return TypeDefName(m.GetDeclaringType()) + "::" + reader.GetString(m.Name);
            }
            case HandleKind.FieldDefinition:
            {
                var f = reader.GetFieldDefinition((FieldDefinitionHandle)h);
                return TypeDefName(f.GetDeclaringType()) + "::" + reader.GetString(f.Name);
            }
            case HandleKind.MemberReference:
            {
                var m = reader.GetMemberReference((MemberReferenceHandle)h);
                return ParentName(m.Parent) + "::" + reader.GetString(m.Name);
            }
            case HandleKind.MethodSpecification:
            {
                var s = reader.GetMethodSpecification((MethodSpecificationHandle)h);
                return MemberName(s.Method) + "<" + string.Join(",", s.DecodeSignature(names, null)) + ">";
            }
            case HandleKind.TypeDefinition:
            case HandleKind.TypeReference:
            case HandleKind.TypeSpecification:
                return ParentName(h);
            case HandleKind.StandaloneSignature:
                return "sig";
        }
        return "?";
    }

    static string Label(int offset) => "IL_" + offset.ToString("x4");

    static JsonArray Disassemble(byte[] il)
    {
        var rows = new JsonArray();
        int i = 0;
        while (i < il.Length)
        {
            int start = i;
            short value = il[i++];
            if (value == 0xFE) value = unchecked((short)(0xFE00 | il[i++]));
            var op = opcodes[value];
            var row = new JsonObject { ["offset"] = start, ["opcode"] = op.Name };
            switch (op.OperandType)
            {
                case OperandType.InlineNone: break;
                case OperandType.ShortInlineBrTarget:
                {
                    sbyte d = (sbyte)il[i++];
                    row["operand"] = Label(i + d);
                    break;
                }
                case OperandType.InlineBrTarget:
                {
                    int d = BitConverter.ToInt32(il, i); i += 4;
                    row["operand"] = Label(i + d);
                    break;
                }
                case OperandType.InlineSwitch:
                {
                    int n = BitConverter.ToInt32(il, i); i += 4;
                    int end = i + 4 * n;
                    var targets = new List<string>();
                    for (int k = 0; k < n; ++k) targets.Add(Label(end + BitConverter.ToInt32(il, i + 4 * k)));
                    i = end;
                    row["operand"] = "(" + string.Join(", ", targets) + ")";
                    break;
                }
                case OperandType.ShortInlineI:
                    row["operand"] = op.Value == OpCodes.Unaligned.Value ? il[i].ToString(CultureInfo.InvariantCulture)
                                                                         : ((sbyte)il[i]).ToString(CultureInfo.InvariantCulture);
                    i += 1;
                    break;
                case OperandType.ShortInlineVar:
                    row["operand"] = il[i++].ToString(CultureInfo.InvariantCulture);
                    break;
                case OperandType.InlineVar:
                    row["operand"] = BitConverter.ToUInt16(il, i).ToString(CultureInfo.InvariantCulture); i += 2;
                    break;
                case OperandType.InlineI:
                    row["operand"] = BitConverter.ToInt32(il, i).ToString(CultureInfo.InvariantCulture); i += 4;
                    break;
                case OperandType.InlineI8:
                    row["operand"] = BitConverter.ToInt64(il, i).ToString(CultureInfo.InvariantCulture); i += 8;
                    break;
                case OperandType.ShortInlineR:
                    row["operand"] = BitConverter.ToSingle(il, i).ToString("G9", CultureInfo.InvariantCulture); i += 4;
                    break;
                case OperandType.InlineR:
                    row["operand"] = BitConverter.ToDouble(il, i).ToString("G17", CultureInfo.InvariantCulture); i += 8;
                    break;
                case OperandType.InlineString:
                {
                    int token = BitConverter.ToInt32(il, i); i += 4;
                    row["operand"] = "\"" + reader.GetUserString(MetadataTokens.UserStringHandle(token & 0xFFFFFF)) + "\"";
                    row["token"] = Token(token);
                    break;
                }
                default: // InlineMethod, InlineField, InlineType, InlineTok, InlineSig
                {
                    int token = BitConverter.ToInt32(il, i); i += 4;
                    row["operand"] = MemberName(MetadataTokens.EntityHandle(token));
                    row["token"] = Token(token);
                    break;
                }
            }
            rows.Add(row);
        }
        return rows;
    }

    static int Main(string[] args)
    {
        using var stream = File.OpenRead(args[0]);
        using var pe = new PEReader(stream);
        reader = pe.GetMetadataReader();
        var root = new JsonObject();

        var asm = reader.GetAssemblyDefinition();
        root["assembly"] = new JsonObject
        {
            ["name"] = reader.GetString(asm.Name),
            ["version"] = asm.Version.ToString(),
            ["culture"] = reader.GetString(asm.Culture),
        };
        var module = reader.GetModuleDefinition();
        root["module"] = new JsonObject { ["name"] = reader.GetString(module.Name), ["mvid"] = reader.GetGuid(module.Mvid).ToString() };
        root["metadata_version"] = reader.MetadataVersion;

        var refs = new JsonArray();
        foreach (var h in reader.AssemblyReferences)
        {
            var r = reader.GetAssemblyReference(h);
            refs.Add(new JsonObject { ["name"] = reader.GetString(r.Name), ["version"] = r.Version.ToString() });
        }
        root["assembly_refs"] = refs;

        var typeRefs = new JsonArray();
        foreach (var h in reader.TypeReferences)
            typeRefs.Add(new JsonObject { ["token"] = Token(h), ["name"] = TypeRefName(h) });
        root["type_refs"] = typeRefs;

        var types = new JsonArray();
        var bodies = new JsonArray();
        foreach (var h in reader.TypeDefinitions)
        {
            var t = reader.GetTypeDefinition(h);
            var fields = new JsonArray();
            foreach (var fh in t.GetFields())
            {
                var f = reader.GetFieldDefinition(fh);
                fields.Add(new JsonObject
                {
                    ["token"] = Token(fh), ["name"] = reader.GetString(f.Name), ["flags"] = (int)f.Attributes,
                    ["signature"] = f.DecodeSignature(names, null),
                });
            }
            var methods = new JsonArray();
            foreach (var mh in t.GetMethods())
            {
                var m = reader.GetMethodDefinition(mh);
                var parameters = new JsonArray();
                foreach (var ph in m.GetParameters())
                {
                    var p = reader.GetParameter(ph);
                    if (p.SequenceNumber > 0) parameters.Add(reader.GetString(p.Name));
                }
                methods.Add(new JsonObject
                {
                    ["token"] = Token(mh), ["name"] = reader.GetString(m.Name), ["flags"] = (int)m.Attributes,
                    ["impl_flags"] = (int)m.ImplAttributes, ["rva"] = m.RelativeVirtualAddress,
                    ["signature"] = MethodSig(m.DecodeSignature(names, null)), ["parameters"] = parameters,
                });
                if (m.RelativeVirtualAddress == 0) continue;
                var body = pe.GetMethodBody(m.RelativeVirtualAddress);
                var clauses = new JsonArray();
                foreach (var c in body.ExceptionRegions)
                    clauses.Add(new JsonObject
                    {
                        ["kind"] = c.Kind.ToString().ToLowerInvariant(),
                        ["try_offset"] = c.TryOffset, ["try_length"] = c.TryLength,
                        ["handler_offset"] = c.HandlerOffset, ["handler_length"] = c.HandlerLength,
                        ["catch_type"] = c.Kind == ExceptionRegionKind.Catch ? ParentName(c.CatchType) : null,
                    });
                bodies.Add(new JsonObject
                {
                    ["token"] = Token(mh), ["method"] = MemberName(mh), ["max_stack"] = body.MaxStack,
                    ["code_size"] = body.GetILBytes().Length, ["init_locals"] = body.LocalVariablesInitialized,
                    ["local_signature"] = body.LocalSignature.IsNil ? null : Token(body.LocalSignature),
                    ["exception_clauses"] = clauses,
                    ["instructions"] = Disassemble(body.GetILBytes()),
                });
            }
            types.Add(new JsonObject
            {
                ["token"] = Token(h), ["name"] = TypeDefName(h), ["flags"] = (int)t.Attributes,
                ["extends"] = t.BaseType.IsNil ? null : ParentName(t.BaseType),
                ["fields"] = fields, ["methods"] = methods,
            });
        }
        root["types"] = types;

        var memberRefs = new JsonArray();
        foreach (var h in reader.MemberReferences)
        {
            var m = reader.GetMemberReference(h);
            string signature = m.GetKind() == MemberReferenceKind.Method
                ? MethodSig(m.DecodeMethodSignature(names, null))
                : m.DecodeFieldSignature(names, null);
            memberRefs.Add(new JsonObject { ["token"] = Token(h), ["name"] = MemberName(h), ["signature"] = signature });
        }
        root["member_refs"] = memberRefs;

        var resources = new JsonArray();
        foreach (var h in reader.ManifestResources)
        {
            var r = reader.GetManifestResource(h);
            resources.Add(new JsonObject { ["name"] = reader.GetString(r.Name), ["offset"] = r.Offset, ["embedded"] = r.Implementation.IsNil });
        }
        root["manifest_resources"] = resources;

        var imports = new JsonArray();
        foreach (var th in reader.TypeDefinitions)
            foreach (var mh in reader.GetTypeDefinition(th).GetMethods())
            {
                var import = reader.GetMethodDefinition(mh).GetImport();
                if (import.Module.IsNil) continue;
                imports.Add(new JsonObject
                {
                    ["method"] = MemberName(mh), ["entry"] = reader.GetString(import.Name),
                    ["module"] = reader.GetString(reader.GetModuleReference(import.Module).Name),
                });
            }
        root["pinvoke"] = imports;
        root["method_bodies"] = bodies;
        var table = new JsonArray();
        foreach (var op in opcodes.Values.OrderBy(o => (ushort)o.Value))
            table.Add(new JsonObject { ["value"] = (int)(ushort)op.Value, ["name"] = op.Name, ["operand"] = op.OperandType.ToString() });
        root["opcodes"] = table;

        Console.WriteLine(root.ToJsonString(new JsonSerializerOptions { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping, TypeInfoResolver = new System.Text.Json.Serialization.Metadata.DefaultJsonTypeInfoResolver() }));
        return 0;
    }
}
