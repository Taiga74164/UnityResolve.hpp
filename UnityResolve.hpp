﻿/*
 * Update: 2024-2-8 13:00
 * Source: https://github.com/issuimo/UnityResolve.hpp
 * Author: github@issuimo
 */

#ifndef UNITYRESOLVE_HPP
#define UNITYRESOLVE_HPP
#define WINDOWS_MODE 1 // 如果需要请改为 1 | 1 if you need
#define ANDROID_MODE 0
#define LINUX_MODE 0
 /* Never
  * #define MAC_MODE 0
  * #define IOS_MODE 0
  */
#if WINDOWS_MODE || LINUX_MODE
#include <format>
#endif
#include <codecvt>
#include <fstream>
#include <iostream>
#include <locale>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#if WINDOWS_MODE
#include <windows.h>
#undef GetObject 
#endif

#if WINDOWS_MODE
#ifdef _WIN64
#define UNITY_CALLING_CONVENTION __fastcall
#elif _WIN32
#define UNITY_CALLING_CONVENTION __cdecl
#endif
#elif ANDROID_MODE || LINUX_MODE
#include <dlfcn.h>
#define UNITY_CALLING_CONVENTION
#endif

class UnityResolve final {
public:
	struct Assembly;
	struct Type;
	struct Class;
	struct Field;
	struct Method;

	enum class Mode : char {
		Il2Cpp,
		Mono,
	};

	struct Assembly final {
		void* address;
		std::string         name;
		std::string         file;
		std::vector<Class*> classes;

		[[nodiscard]] auto Get(const std::string& strClass, const std::string& strNamespace = "*", const std::string& strParent = "*") const -> Class* {
			for (const auto pClass : classes) if (strClass == pClass->name && (strNamespace == "*" || pClass->namespaze == strNamespace) && (strParent == "*" || pClass->parent == strParent)) return pClass;
			return nullptr;
		}
	};

	struct Type final {
		void* address;
		std::string name;
		int         size;

		[[nodiscard]] auto GetObject() const -> void* {
			if (mode_ == Mode::Il2Cpp) return Invoke<void*>("il2cpp_type_get_object", address);
			return Invoke<void*>("mono_type_get_object", pDomain, address);
		}
	};

	struct Class final {
		void* classinfo;
		std::string          name;
		std::string          parent;
		std::string          namespaze;
		std::vector<Field*>  fields;
		std::vector<Method*> methods;

		template <typename RType>
		auto Get(const std::string& name, const std::vector<std::string> args = {}) -> RType* {
			if constexpr (std::is_same_v<RType, Field>) for (auto pField : fields) if (pField->name == name) return static_cast<RType*>(pField);
			if constexpr (std::is_same_v<RType, std::int32_t>) for (const auto pField : fields) if (pField->name == name) return reinterpret_cast<RType*>(pField->offset);
			if constexpr (std::is_same_v<RType, Method>) {
				for (auto pMethod : methods) {
					if (pMethod->name == name) {
						if (pMethod->args.size() == 0 && args.size() == 0) {
							return static_cast<RType*>(pMethod);
						}
						if (pMethod->args.size() == args.size()) {
							for (size_t index{ 0 }; const auto & typeName : args)
							if (typeName == "*" || typeName.empty() ? false : pMethod->args[index++]->pType->name != typeName) {
								return static_cast<RType*>(pMethod);
							}
						}
					}
				}
			}
			return nullptr;
		}

		template <typename RType>
		auto GetValue(void* obj, const std::string& name) -> RType { return *reinterpret_cast<RType*>(reinterpret_cast<uintptr_t>(obj) + Get<Field>(name)->offset); }

		template <typename RType>
		auto SetValue(void* obj, const std::string& name, RType value) -> void { return *reinterpret_cast<RType*>(reinterpret_cast<uintptr_t>(obj) + Get<Field>(name)->offset) = value; }

		[[nodiscard]] auto GetType() const -> Type {
			if (mode_ == Mode::Il2Cpp) {
				const auto pUType = Invoke<void*, void*>("il2cpp_class_get_type", classinfo);
				return { pUType, name, -1 };
			}
			const auto pUType = Invoke<void*, void*>("mono_class_get_type", classinfo);
			return { pUType, name, -1 };
		}

		/**
		 * \brief 获取类所有实例
		 * \tparam T 返回数组类型
		 * \param type 类
		 * \return 返回实例指针数组
		 */
		template <typename T>
		auto FindObjectsByType() -> std::vector<T> {
			static Method* pMethod;

			if (!pMethod) pMethod = UnityResolve::Get("UnityEngine.CoreModule.dll")->Get("Object")->Get<Method>(mode_ == Mode::Il2Cpp ? "FindObjectsOfType" : "FindObjectsOfTypeAll", { "System.Type" });

			if (pMethod) {
				std::vector<T> rs{};
				auto           array = pMethod->Invoke<UnityType::Array<T>*>(this->GetType().GetObject());
				rs.reserve(array->max_length);
				for (auto i = 0; i < array->max_length; i++) rs.push_back(array->At(i));
				return rs;
			}

			throw std::logic_error("FindObjectsOfType nullptr");
		}

		template <typename T>
		auto New() -> T* {
			if (mode_ == Mode::Il2Cpp) return Invoke<T*, void*>("il2cpp_object_new", classinfo);
			return Invoke<T*, void*, void*>("mono_object_new", pDomain, classinfo);
		}
	};

	struct Field final {
		void* fieldinfo;
		std::string  name;
		Type* type;
		Class* klass;
		std::int32_t offset; // If offset is -1, then it's thread static
		bool         static_field;
		void* vTable;

		template <typename T>
		auto SetValue(T* value) const -> void {
			if (!static_field) return;
			if (mode_ == Mode::Il2Cpp) return Invoke<void, void*, T*>("il2cpp_field_static_set_value", fieldinfo, value);
		}

		template <typename T>
		auto GetValue(T* value) const -> void {
			if (!static_field) return;
			if (mode_ == Mode::Il2Cpp) return Invoke<void, void*, T*>("il2cpp_field_static_get_value", fieldinfo, value);
		}
	};

	struct Method final {
		void* address;
		std::string  name;
		Class* klass;
		Type* return_type;
		std::int32_t flags;
		bool         static_function;
		void* function;

		struct Arg {
			std::string name;
			Type* pType;
		};

		std::vector<Arg*> args;

	private:
		bool badPtr{ false };
	public:

		template <typename Return, typename... Args>
		auto Invoke(Args... args) -> Return {
			Compile();
#if WINDOWS_MODE
			try {
				if (!badPtr) badPtr = !IsBadCodePtr(static_cast<FARPROC>(function));
				if (function && badPtr) return reinterpret_cast<Return(UNITY_CALLING_CONVENTION*)(Args...)>(function)(args...);
			}
			catch (...) {}
#else
			if (function) return reinterpret_cast<Return(UNITY_CALLING_CONVENTION*)(Args...)>(function)(args...);
#endif
			return Return();
		}

		auto Compile() -> void { if (address && !function && mode_ == Mode::Mono) function = UnityResolve::Invoke<void*>("mono_compile_method", address); }

		template <typename Return, typename Obj, typename... Args>
		auto RuntimeInvoke(Obj* obj, Args... args) -> Return {
			void* exc{};
			void* argArray[sizeof...(Args) + 1];
			if (sizeof...(Args) > 0) {
				size_t index = 0;
				((argArray[index++] = static_cast<void*>(&args)), ...);
			}

			if (mode_ == Mode::Il2Cpp) {
				if constexpr (std::is_same_v<Return, void>) {
					UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", address, obj, argArray, exc);
					return;
				}
				else return *static_cast<Return*>(UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", address, obj, argArray, exc));
			}

			if constexpr (std::is_same_v<Return, void>) {
				UnityResolve::Invoke<void*>("mono_runtime_invoke", address, obj, argArray, exc);
				return;
			}
			else return *static_cast<Return*>(UnityResolve::Invoke<void*>("mono_runtime_invoke", address, obj, argArray, exc));
		}

		template <typename Return, typename... Args>
		using MethodPointer = Return(UNITY_CALLING_CONVENTION*)(Args...);

		template <typename Return, typename... Args>
		auto Cast() -> MethodPointer<Return, Args...> {
			Compile();
			if (function) return static_cast<MethodPointer<Return, Args...>>(function);
			throw std::logic_error("nullptr");
		}
	};

	static auto ThreadAttach() -> void {
		if (mode_ == Mode::Il2Cpp) Invoke<void*>("il2cpp_thread_attach", pDomain);
		else {
			Invoke<void*>("mono_thread_attach", pDomain);
			Invoke<void*>("mono_jit_thread_attach", pDomain);
		}
	}

	static auto ThreadDetach() -> void {
		if (mode_ == Mode::Il2Cpp) Invoke<void*>("il2cpp_thread_detach", pDomain);
		else {
			Invoke<void*>("mono_thread_detach", pDomain);
			Invoke<void*>("mono_jit_thread_detach", pDomain);
		}
	}

	static auto Init(void* hmodule, const Mode mode = Mode::Mono) -> void {
		mode_ = mode;
		hmodule_ = hmodule;

		if (mode_ == Mode::Il2Cpp) {
			pDomain = Invoke<void*>("il2cpp_domain_get");
			Invoke<void*>("il2cpp_thread_attach", pDomain);
			ForeachAssembly();
		}
		else {
			pDomain = Invoke<void*>("mono_get_root_domain");
			Invoke<void*>("mono_thread_attach", pDomain);
			Invoke<void*>("mono_jit_thread_attach", pDomain);

			ForeachAssembly();

			if (Get("UnityEngine.dll") && (!Get("UnityEngine.CoreModule.dll") || !Get("UnityEngine.PhysicsModule.dll"))) {
				// 兼容某些游戏 (如生死狙击2)
				for (const std::vector<std::string> names = { "UnityEngine.CoreModule.dll", "UnityEngine.PhysicsModule.dll" }; const auto name : names) {
					const auto ass = Get("UnityEngine.dll");
					const auto assembly = new Assembly{ .address = ass->address, .name = name, .file = ass->file, .classes = ass->classes };
					UnityResolve::assembly.push_back(assembly);
				}
			}
		}
	}

#if WINDOWS_MODE || LINUX_MODE /*__cplusplus >= 202002L*/
	static auto DumpToFile(const std::string& file) -> void {
		std::ofstream io(file, std::fstream::out);

		if (!io) return;

		for (const auto& pAssembly : assembly) {
			for (const auto& pClass : pAssembly->classes) {
				io << std::format("\tnamespace: {}", pClass->namespaze.empty() ? "" : pClass->namespaze);
				io << "\n";
				io << std::format("\tAssembly: {}\n", pAssembly->name.empty() ? "" : pAssembly->name);
				io << std::format("\tAssemblyFile: {} \n", pAssembly->file.empty() ? "" : pAssembly->file);
				io << std::format("\tclass {}{} ", pClass->name, pClass->parent.empty() ? "" : " : " + pClass->parent);
				io << "{\n\n";
				for (const auto& pField : pClass->fields) io << std::format("\t\t{:+#06X} | {}{} {}\n", pField->offset, pField->static_field ? "static " : "", pField->type->name, pField->name);
				io << "\n";
				for (const auto& pMethod : pClass->methods) {
					io << std::format("\t\t[Flags: {:032b}] [ParamsCount: {:04d}] |RVA: {:+#010X}|\n", pMethod->flags, pMethod->args.size(), reinterpret_cast<std::uint64_t>(pMethod->function) - reinterpret_cast<std::uint64_t>(hmodule_));
					io << std::format("\t\t{}{} {}(", pMethod->static_function ? "static " : "", pMethod->return_type->name, pMethod->name);
					std::string params{};
					for (const auto& pArg : pMethod->args) params += std::format("{} {}, ", pArg->pType->name, pArg->name);
					if (!params.empty()) {
						params.pop_back();
						params.pop_back();
					}
					io << (params.empty() ? "" : params) << ");\n\n";
				}
				io << "\t}\n\n";
			}
		}

		io << '\n';
		io.close();
	}
#endif

	/**
	 * \brief 调用dll函数
	 * \tparam Return 返回类型 (必须)
	 * \tparam Args 参数类型 (可以忽略)
	 * \param funcName dll导出函数名称
	 * \param args 参数
	 * \return 模板类型
	 */
	template <typename Return, typename... Args>
	static auto Invoke(const std::string& funcName, Args... args) -> Return {
		static std::mutex mutex{};
		std::lock_guard   lock(mutex);

		// 检查函数是否已经获取地址, 没有则自动获取
		if (!address_.contains(funcName) || address_[funcName] == nullptr) {
#if WINDOWS_MODE
			address_[funcName] = static_cast<void*>(GetProcAddress(static_cast<HMODULE>(hmodule_), funcName.c_str()));
#elif  ANDROID_MODE || LINUX_MODE
			address_[funcName] = dlsym(hmodule_, funcName.c_str());
#endif
		}

		if (address_[funcName] != nullptr) return reinterpret_cast<Return(UNITY_CALLING_CONVENTION*)(Args...)>(address_[funcName])(args...);
		throw std::logic_error("Not find function");
	}

	inline static std::vector<Assembly*> assembly;

	static auto Get(const std::string& strAssembly) -> Assembly* {
		for (const auto pAssembly : assembly) if (pAssembly->name == strAssembly) return pAssembly;
		return nullptr;
	}

private:
	static auto ForeachAssembly() -> void {
		// 遍历程序集
		if (mode_ == Mode::Il2Cpp) {
			size_t     nrofassemblies = 0;
			const auto assemblies = Invoke<void**>("il2cpp_domain_get_assemblies", pDomain, &nrofassemblies);
			for (auto i = 0; i < nrofassemblies; i++) {
				const auto ptr = assemblies[i];
				if (ptr == nullptr) continue;
				auto       assembly = new Assembly{ .address = ptr };
				const auto image = Invoke<void*>("il2cpp_assembly_get_image", ptr);
				assembly->file = Invoke<const char*>("il2cpp_image_get_filename", image);
				assembly->name = Invoke<const char*>("il2cpp_image_get_name", image);
				UnityResolve::assembly.push_back(assembly);
				ForeachClass(assembly, image);
			}
		}
		else {
			Invoke<void*, void(*)(void* ptr, std::vector<Assembly*>&), std::vector<Assembly*>&>("mono_assembly_foreach",
				[](void* ptr, std::vector<Assembly*>& v) {
					if (ptr == nullptr) return;

					const auto assembly = new Assembly{ .address = ptr, };
					const auto image = Invoke<void*>("mono_assembly_get_image", ptr);
					assembly->file = Invoke<const char*>("mono_image_get_filename", image);
					assembly->name = Invoke<const char*>("mono_image_get_name", image);
					assembly->name += ".dll";
					v.push_back(assembly);

					ForeachClass(assembly, image);
				},
				assembly);
		}
	}

	static auto ForeachClass(Assembly* assembly, void* image) -> void {
		// 遍历类
		if (mode_ == Mode::Il2Cpp) {
			const auto count = Invoke<int>("il2cpp_image_get_class_count", image);
			for (auto i = 0; i < count; i++) {
				const auto pClass = Invoke<void*>("il2cpp_image_get_class", image, i);
				if (pClass == nullptr) continue;
				const auto pAClass = new Class();
				pAClass->classinfo = pClass;
				pAClass->name = Invoke<const char*>("il2cpp_class_get_name", pClass);
				if (const auto pPClass = Invoke<void*>("il2cpp_class_get_parent", pClass)) pAClass->parent = Invoke<const char*>("il2cpp_class_get_name", pPClass);
				pAClass->namespaze = Invoke<const char*>("il2cpp_class_get_namespace", pClass);
				assembly->classes.push_back(pAClass);

				ForeachFields(pAClass, pClass);
				ForeachMethod(pAClass, pClass);

				void* i_class{};
				void* iter{};
				do {
					if ((i_class = Invoke<void*>("il2cpp_class_get_interfaces", pClass, &iter))) {
						ForeachFields(pAClass, i_class);
						ForeachMethod(pAClass, i_class);
					}
				} while (i_class);
			}
		}
		else {
			const void* table = Invoke<void*>("mono_image_get_table_info", image, 2);
			const auto  count = Invoke<int>("mono_table_info_get_rows", table);
			for (auto i = 0; i < count; i++) {
				const auto pClass = Invoke<void*>("mono_class_get", image, 0x02000000 | (i + 1));
				if (pClass == nullptr) continue;

				const auto pAClass = new Class();
				pAClass->classinfo = pClass;
				pAClass->name = Invoke<const char*>("mono_class_get_name", pClass);
				if (const auto pPClass = Invoke<void*>("mono_class_get_parent", pClass)) pAClass->parent = Invoke<const char*>("mono_class_get_name", pPClass);
				pAClass->namespaze = Invoke<const char*>("mono_class_get_namespace", pClass);
				assembly->classes.push_back(pAClass);

				ForeachFields(pAClass, pClass);
				ForeachMethod(pAClass, pClass);

				void* iClass{};
				void* iiter{};

				do {
					if ((iClass = Invoke<void*>("mono_class_get_interfaces", pClass, &iiter))) {
						ForeachFields(pAClass, iClass);
						ForeachMethod(pAClass, iClass);
					}
				} while (iClass);
			}
		}
	}

	static auto ForeachFields(Class* klass, void* pKlass) -> void {
		// 遍历成员
		if (mode_ == Mode::Il2Cpp) {
			void* iter = nullptr;
			void* field;
			do {
				if ((field = Invoke<void*>("il2cpp_class_get_fields", pKlass, &iter))) {
					const auto pField = new Field{ .fieldinfo = field, .name = Invoke<const char*>("il2cpp_field_get_name", field), .type = new Type{.address = Invoke<void*>("il2cpp_field_get_type", field)}, .klass = klass, .offset = Invoke<int>("il2cpp_field_get_offset", field), .static_field = false, .vTable = nullptr };
					int        tSize{};
					pField->static_field = pField->offset <= 0;
					pField->type->name = Invoke<const char*>("il2cpp_type_get_name", pField->type->address);
					pField->type->size = -1;
					klass->fields.push_back(pField);
				}
			} while (field);
		}
		else {
			void* iter = nullptr;
			void* field;
			do {
				if ((field = Invoke<void*>("mono_class_get_fields", pKlass, &iter))) {
					const auto pField = new Field{ .fieldinfo = field, .name = Invoke<const char*>("mono_field_get_name", field), .type = new Type{.address = Invoke<void*>("mono_field_get_type", field)}, .klass = klass, .offset = Invoke<int>("mono_field_get_offset", field), .static_field = false, .vTable = nullptr };
					int        tSize{};
					pField->static_field = pField->offset <= 0;
					pField->type->name = Invoke<const char*>("mono_type_get_name", pField->type->address);
					pField->type->size = Invoke<int>("mono_type_size", pField->type->address, &tSize);
					klass->fields.push_back(pField);
				}
			} while (field);
		}
	}

	static auto ForeachMethod(Class* klass, void* pKlass) -> void {
		// 遍历方法
		if (mode_ == Mode::Il2Cpp) {
			void* iter = nullptr;
			void* method;
			do {
				if ((method = Invoke<void*>("il2cpp_class_get_methods", pKlass, &iter))) {
					int        fFlags{};
					const auto pMethod = new Method{};
					pMethod->address = method;
					pMethod->name = Invoke<const char*>("il2cpp_method_get_name", method);
					pMethod->klass = klass;
					pMethod->return_type = new Type{ .address = Invoke<void*>("il2cpp_method_get_return_type", method), };
					pMethod->flags = Invoke<int>("il2cpp_method_get_flags", method, &fFlags);

					int        tSize{};
					pMethod->static_function = pMethod->flags & 0x10;
					pMethod->return_type->name = Invoke<const char*>("il2cpp_type_get_name", pMethod->return_type->address);
					pMethod->return_type->size = -1;
					pMethod->function = *static_cast<void**>(method);
					klass->methods.push_back(pMethod);
					const auto argCount = Invoke<int>("il2cpp_method_get_param_count", method);
					for (auto index = 0; index < argCount; index++) pMethod->args.push_back(new Method::Arg{ Invoke<const char*>("il2cpp_method_get_param_name", method, index), new Type{.address = Invoke<void*>("il2cpp_method_get_param", method, index), .name = Invoke<const char*>("il2cpp_type_get_name", Invoke<void*>("il2cpp_method_get_param", method, index)), .size = -1} });
				}
			} while (method);
		}
		else {
			void* iter = nullptr;
			void* method;
			do {
				if ((method = Invoke<void*>("mono_class_get_methods", pKlass, &iter))) {
					const auto signature = Invoke<void*>("mono_method_signature", method);
					int        fFlags{};
					const auto pMethod = new Method{};
					pMethod->address = method;
					pMethod->name = Invoke<const char*>("mono_method_get_name", method);
					pMethod->klass = klass;
					pMethod->return_type = new Type{ .address = Invoke<void*>("mono_signature_get_return_type", method), };
					pMethod->flags = Invoke<int>("mono_method_get_flags", method, &fFlags);
					int        tSize{};
					pMethod->static_function = pMethod->flags & 0x10;
					pMethod->return_type->name = Invoke<const char*>("mono_type_get_name", pMethod->return_type->address);
					pMethod->return_type->size = Invoke<int>("mono_type_size", pMethod->return_type->address, &tSize);
					klass->methods.push_back(pMethod);

					const auto names = new char* [Invoke<int>("mono_signature_get_param_count", signature)];
					Invoke<void>("mono_method_get_param_names", method, names);

					void* mIter = nullptr;
					void* mType;
					auto  iname = 0;
					do {
						if ((mType = Invoke<void*>("mono_signature_get_params", signature, &mIter))) {
							int t_size{};
							pMethod->args.push_back(new Method::Arg{ names[iname], new Type{.address = mType, .name = Invoke<const char*>("mono_type_get_name", mType), .size = Invoke<int>("mono_type_size", mType, &t_size)} });
							iname++;
						}
					} while (mType);
				}
			} while (method);
		}
	}

public:
	class UnityType final {
	public:
		struct Vector3;
		struct Camera;
		struct Transform;
		struct Component;
		struct UnityObject;
		struct LayerMask;
		struct Rigidbody;
		struct Physics;
		struct GameObject;
		struct Collider;
		struct Vector4;
		struct Vector2;
		struct Quaternion;
		struct Bounds;
		struct Plane;
		struct Ray;
		struct Rect;
		struct Color;
		struct Matrix4x4;
		template <typename T>
		struct Array;
		struct String;
		struct Object;
		template <typename T>
		struct List;
		template <typename TKey, typename TValue>
		struct Dictionary;
		struct Behaviour;
		struct MonoBehaviour;
		struct CsType;
		struct Mesh;
		struct Renderer;
		struct Animator;
		struct CapsuleCollider;
		struct BoxCollider;

		struct Vector3 {
			float x, y, z;

			Vector3() { x = y = z = 0.f; }

			Vector3(const float f1, const float f2, const float f3) {
				x = f1;
				y = f2;
				z = f3;
			}

			[[nodiscard]] auto Length() const -> float { return x * x + y * y + z * z; }

			[[nodiscard]] auto Dot(const Vector3 b) const -> float { return x * b.x + y * b.y + z * b.z; }

			[[nodiscard]] auto Normalize() const -> Vector3 {
				if (const auto len = Length(); len > 0) return Vector3(x / len, y / len, z / len);
				return Vector3(x, y, z);
			}

			auto ToVectors(Vector3* m_pForward, Vector3* m_pRight, Vector3* m_pUp) const -> void {
				constexpr auto m_fDeg2Rad = static_cast<float>(3.1415926) / 180.F;

				const auto m_fSinX = sinf(x * m_fDeg2Rad);
				const auto m_fCosX = cosf(x * m_fDeg2Rad);

				const auto m_fSinY = sinf(y * m_fDeg2Rad);
				const auto m_fCosY = cosf(y * m_fDeg2Rad);

				const auto m_fSinZ = sinf(z * m_fDeg2Rad);
				const auto m_fCosZ = cosf(z * m_fDeg2Rad);

				if (m_pForward) {
					m_pForward->x = m_fCosX * m_fCosY;
					m_pForward->y = -m_fSinX;
					m_pForward->z = m_fCosX * m_fSinY;
				}

				if (m_pRight) {
					m_pRight->x = -1.f * m_fSinZ * m_fSinX * m_fCosY + -1.f * m_fCosZ * -m_fSinY;
					m_pRight->y = -1.f * m_fSinZ * m_fCosX;
					m_pRight->z = -1.f * m_fSinZ * m_fSinX * m_fSinY + -1.f * m_fCosZ * m_fCosY;
				}

				if (m_pUp) {
					m_pUp->x = m_fCosZ * m_fSinX * m_fCosY + -m_fSinZ * -m_fSinY;
					m_pUp->y = m_fCosZ * m_fCosX;
					m_pUp->z = m_fCosZ * m_fSinX * m_fSinY + -m_fSinZ * m_fCosY;
				}
			}

			[[nodiscard]] auto Distance(const Vector3& event) const -> float {
				const auto dx = this->x - event.x;
				const auto dy = this->y - event.y;
				const auto dz = this->z - event.z;
				return std::sqrt(dx * dx + dy * dy + dz * dz);
			}

			auto operator*(const float x) -> Vector3 {
				this->x *= x;
				this->y *= x;
				this->z *= x;
				return *this;
			}

			auto operator-(const float x) -> Vector3 {
				this->x -= x;
				this->y -= x;
				this->z -= x;
				return *this;
			}

			auto operator+(const float x) -> Vector3 {
				this->x += x;
				this->y += x;
				this->z += x;
				return *this;
			}

			auto operator/(const float x) -> Vector3 {
				this->x /= x;
				this->y /= x;
				this->z /= x;
				return *this;
			}

			auto operator*(const Vector3 x) -> Vector3 {
				this->x *= x.x;
				this->y *= x.y;
				this->z *= x.z;
				return *this;
			}

			auto operator-(const Vector3 x) -> Vector3 {
				this->x -= x.x;
				this->y -= x.y;
				this->z -= x.z;
				return *this;
			}

			auto operator+(const Vector3 x) -> Vector3 {
				this->x += x.x;
				this->y += x.y;
				this->z += x.z;
				return *this;
			}

			auto operator/(const Vector3 x) -> Vector3 {
				this->x /= x.x;
				this->y /= x.y;
				this->z /= x.z;
				return *this;
			}
		};

		struct Vector2 {
			float x, y;

			Vector2() { x = y = 0.f; }

			Vector2(const float f1, const float f2) {
				x = f1;
				y = f2;
			}

			[[nodiscard]] auto Distance(const Vector2& event) const -> float {
				const auto dx = this->x - event.x;
				const auto dy = this->y - event.y;
				return std::sqrt(dx * dx + dy * dy);
			}

			auto operator*(const float x) -> Vector2 {
				this->x *= x;
				this->y *= x;
				return *this;
			}

			auto operator/(const float x) -> Vector2 {
				this->x /= x;
				this->y /= x;
				return *this;
			}

			auto operator+(const float x) -> Vector2 {
				this->x += x;
				this->y += x;
				return *this;
			}

			auto operator-(const float x) -> Vector2 {
				this->x -= x;
				this->y -= x;
				return *this;
			}

			auto operator*(const Vector2 x) -> Vector2 {
				this->x *= x.x;
				this->y *= x.y;
				return *this;
			}

			auto operator-(const Vector2 x) -> Vector2 {
				this->x -= x.x;
				this->y -= x.y;
				return *this;
			}

			auto operator+(const Vector2 x) -> Vector2 {
				this->x += x.x;
				this->y += x.y;
				return *this;
			}

			auto operator/(const Vector2 x) -> Vector2 {
				this->x /= x.x;
				this->y /= x.y;
				return *this;
			}
		};

		struct Vector4 {
			float x, y, z, w;

			Vector4() { x = y = z = w = 0.F; }

			Vector4(const float f1, const float f2, const float f3, const float f4) {
				x = f1;
				y = f2;
				z = f3;
				w = f4;
			}

			auto operator*(const float x) -> Vector4 {
				this->x *= x;
				this->y *= x;
				this->z *= x;
				this->w *= x;
				return *this;
			}

			auto operator-(const float x) -> Vector4 {
				this->x -= x;
				this->y -= x;
				this->z -= x;
				this->w -= x;
				return *this;
			}

			auto operator+(const float x) -> Vector4 {
				this->x += x;
				this->y += x;
				this->z += x;
				this->w += x;
				return *this;
			}

			auto operator/(const float x) -> Vector4 {
				this->x /= x;
				this->y /= x;
				this->z /= x;
				this->w /= x;
				return *this;
			}

			auto operator*(const Vector4 x) -> Vector4 {
				this->x *= x.x;
				this->y *= x.y;
				this->z *= x.z;
				this->w *= x.w;
				return *this;
			}

			auto operator-(const Vector4 x) -> Vector4 {
				this->x -= x.x;
				this->y -= x.y;
				this->z -= x.z;
				this->w -= x.w;
				return *this;
			}

			auto operator+(const Vector4 x) -> Vector4 {
				this->x += x.x;
				this->y += x.y;
				this->z += x.z;
				this->w += x.w;
				return *this;
			}

			auto operator/(const Vector4 x) -> Vector4 {
				this->x /= x.x;
				this->y /= x.y;
				this->z /= x.z;
				this->w /= x.w;
				return *this;
			}
		};

		struct Quaternion {
			float x, y, z, w;

			Quaternion() { x = y = z = w = 0.F; }

			Quaternion(const float f1, const float f2, const float f3, const float f4) {
				x = f1;
				y = f2;
				z = f3;
				w = f4;
			}

			auto Euler(float m_fX, float m_fY, float m_fZ) -> Quaternion {
				constexpr auto m_fDeg2Rad = static_cast<float>(3.1415926) / 180.F;

				m_fX = m_fX * m_fDeg2Rad * 0.5F;
				m_fY = m_fY * m_fDeg2Rad * 0.5F;
				m_fZ = m_fZ * m_fDeg2Rad * 0.5F;

				const auto m_fSinX = sinf(m_fX);
				const auto m_fCosX = cosf(m_fX);

				const auto m_fSinY = sinf(m_fY);
				const auto m_fCosY = cosf(m_fY);

				const auto m_fSinZ = sinf(m_fZ);
				const auto m_fCosZ = cosf(m_fZ);

				x = m_fCosY * m_fSinX * m_fCosZ + m_fSinY * m_fCosX * m_fSinZ;
				y = m_fSinY * m_fCosX * m_fCosZ - m_fCosY * m_fSinX * m_fSinZ;
				z = m_fCosY * m_fCosX * m_fSinZ - m_fSinY * m_fSinX * m_fCosZ;
				w = m_fCosY * m_fCosX * m_fCosZ + m_fSinY * m_fSinX * m_fSinZ;

				return *this;
			}

			auto Euler(const Vector3& m_vRot) -> Quaternion { return Euler(m_vRot.x, m_vRot.y, m_vRot.z); }

			[[nodiscard]] auto ToEuler() const -> Vector3 {
				Vector3 m_vEuler;

				const auto m_fDist = (x * x) + (y * y) + (z * z) + (w * w);

				if (const auto m_fTest = x * w - y * z; m_fTest > 0.4995F * m_fDist) {
					m_vEuler.x = static_cast<float>(3.1415926) * 0.5F;
					m_vEuler.y = 2.F * atan2f(y, x);
					m_vEuler.z = 0.F;
				}
				else if (m_fTest < -0.4995F * m_fDist) {
					m_vEuler.x = static_cast<float>(3.1415926) * -0.5F;
					m_vEuler.y = -2.F * atan2f(y, x);
					m_vEuler.z = 0.F;
				}
				else {
					m_vEuler.x = asinf(2.F * (w * x - y * z));
					m_vEuler.y = atan2f(2.F * w * y + 2.F * z * x, 1.F - 2.F * (x * x + y * y));
					m_vEuler.z = atan2f(2.F * w * z + 2.F * x * y, 1.F - 2.F * (z * z + x * x));
				}

				constexpr auto m_fRad2Deg = 180.F / static_cast<float>(3.1415926);
				m_vEuler.x *= m_fRad2Deg;
				m_vEuler.y *= m_fRad2Deg;
				m_vEuler.z *= m_fRad2Deg;

				return m_vEuler;
			}

			auto operator*(const float x) -> Quaternion {
				this->x *= x;
				this->y *= x;
				this->z *= x;
				this->w *= x;
				return *this;
			}

			auto operator-(const float x) -> Quaternion {
				this->x -= x;
				this->y -= x;
				this->z -= x;
				this->w -= x;
				return *this;
			}

			auto operator+(const float x) -> Quaternion {
				this->x += x;
				this->y += x;
				this->z += x;
				this->w += x;
				return *this;
			}

			auto operator/(const float x) -> Quaternion {
				this->x /= x;
				this->y /= x;
				this->z /= x;
				this->w /= x;
				return *this;
			}

			auto operator*(const Quaternion x) -> Quaternion {
				this->x *= x.x;
				this->y *= x.y;
				this->z *= x.z;
				this->w *= x.w;
				return *this;
			}

			auto operator-(const Quaternion x) -> Quaternion {
				this->x -= x.x;
				this->y -= x.y;
				this->z -= x.z;
				this->w -= x.w;
				return *this;
			}

			auto operator+(const Quaternion x) -> Quaternion {
				this->x += x.x;
				this->y += x.y;
				this->z += x.z;
				this->w += x.w;
				return *this;
			}

			auto operator/(const Quaternion x) -> Quaternion {
				this->x /= x.x;
				this->y /= x.y;
				this->z /= x.z;
				this->w /= x.w;
				return *this;
			}
		};

		struct Bounds {
			Vector3 m_vCenter;
			Vector3 m_vExtents;
		};

		struct Plane {
			Vector3 m_vNormal;
			float   fDistance;
		};

		struct Ray {
			Vector3 m_vOrigin;
			Vector3 m_vDirection;
		};

		struct Rect {
			float fX, fY;
			float fWidth, fHeight;

			Rect() { fX = fY = fWidth = fHeight = 0.f; }

			Rect(const float f1, const float f2, const float f3, const float f4) {
				fX = f1;
				fY = f2;
				fWidth = f3;
				fHeight = f4;
			}
		};

		struct Color {
			float r, g, b, a;

			Color() { r = g = b = a = 0.f; }

			explicit Color(const float fRed = 0.f, const float fGreen = 0.f, const float fBlue = 0.f, const float fAlpha = 1.f) {
				r = fRed;
				g = fGreen;
				b = fBlue;
				a = fAlpha;
			}
		};

		struct Matrix4x4 {
			float m[4][4] = { {0} };

			Matrix4x4() = default;

			auto operator[](const int i) -> float* { return m[i]; }
		};

		struct Object {
			union {
				void* klass{ nullptr };
				void* vtable;
			} Il2CppClass;

			struct MonitorData* monitor{ nullptr };

			auto GetType() -> CsType* {
				static Method* method;
				if (!method) method = Get("mscorlib.dll")->Get("Object")->Get<Method>("GetType");
				if (method) return method->Invoke<CsType*>(this);
				throw std::logic_error("nullptr");
			}

			auto ToString() -> std::string {
				static Method* method;
				if (!method) method = Get("mscorlib.dll")->Get("Object")->Get<Method>("ToString");
				if (method) return method->Invoke<String*>(this)->ToString();
				throw std::logic_error("nullptr");
			}
		};

		struct CsType {
			auto FormatTypeName() -> std::string {
				static Method* method;
				if (!method) method = Get("mscorlib.dll")->Get("Type")->Get<Method>("FormatTypeName");
				if (method) return method->Invoke<String*>(this)->ToString();
				throw std::logic_error("nullptr");
			}

			auto GetFullName() -> std::string {
				static Method* method;
				if (!method) method = Get("mscorlib.dll")->Get("Type")->Get<Method>("get_FullName");
				if (method) return method->Invoke<String*>(this)->ToString();
				throw std::logic_error("nullptr");
			}

			auto GetNamespace() -> std::string {
				static Method* method;
				if (!method) method = Get("mscorlib.dll")->Get("Type")->Get<Method>("get_Namespace");
				if (method) return method->Invoke<String*>(this)->ToString();
				throw std::logic_error("nullptr");
			}
		};

		struct String : Object {
			int32_t m_stringLength{ 0 };
			wchar_t m_firstChar[32]{};

			[[nodiscard]] auto ToString() const -> std::string {
#if WINDOWS_MODE
				std::string sRet(static_cast<size_t>(m_stringLength) * 3 + 1, '\0');
				WideCharToMultiByte(CP_UTF8, 0, m_firstChar, m_stringLength, sRet.data(), static_cast<int>(sRet.size()), nullptr, nullptr);
				return sRet;
#elif LINUX_MODE
				using convert_typeX = std::codecvt_utf8<wchar_t>;
				std::wstring_convert<convert_typeX, wchar_t> converterX;
				return converterX.to_bytes(m_firstChar);
#elif ANDROID_MODE
				// 可能存在bug 目前已有报告 "比如对象标签 有的时候会直接跳到游戏控制器里面去"
				using convert_typeX = std::codecvt_utf8<wchar_t>;
				std::wstring_convert<convert_typeX, wchar_t> converterX;
				return converterX.to_bytes(m_firstChar);
#endif
			}

			auto operator[](const int i) const -> wchar_t { return m_firstChar[i]; }

			auto Clear() -> void {
				memset(m_firstChar, 0, m_stringLength);
				m_stringLength = 0;
			}

			static auto New(const std::string& str) -> String* {
				if (mode_ == Mode::Il2Cpp) return UnityResolve::Invoke<String*, const char*>("il2cpp_string_new", str.c_str());
				return UnityResolve::Invoke<String*, void*, const char*>("mono_string_new", UnityResolve::Invoke<void*>("mono_get_root_domain"), str.c_str());
			}
		};

		template <typename T>
		struct Array : Object {
			struct {
				std::uintptr_t length;
				std::int32_t   lower_bound;
			}*bounds{ nullptr };

			std::uintptr_t          max_length{ 0 };
			__declspec(align(8)) T* vector[32]{};

			auto GetData() -> uintptr_t { return reinterpret_cast<uintptr_t>(&vector); }

			auto operator[](const unsigned int m_uIndex) -> T& { return *reinterpret_cast<T*>(GetData() + sizeof(T) * m_uIndex); }

			auto At(const unsigned int m_uIndex) -> T& { return operator[](m_uIndex); }

			auto Insert(T* m_pArray, uintptr_t m_uSize, const uintptr_t m_uIndex = 0) -> void {
				if ((m_uSize + m_uIndex) >= max_length) {
					if (m_uIndex >= max_length) return;

					m_uSize = max_length - m_uIndex;
				}

				for (uintptr_t u = 0; m_uSize > u; ++u) operator[](u + m_uIndex) = m_pArray[u];
			}

			auto Fill(T m_tValue) -> void { for (uintptr_t u = 0; max_length > u; ++u) operator[](u) = m_tValue; }

			auto RemoveAt(const unsigned int m_uIndex) -> void {
				if (m_uIndex >= max_length) return;

				if (max_length > (m_uIndex + 1)) for (auto u = m_uIndex; (static_cast<unsigned int>(max_length) - m_uIndex) > u; ++u) operator[](u) = operator[](u + 1);

				--max_length;
			}

			auto RemoveRange(const unsigned int m_uIndex, unsigned int m_uCount) -> void {
				if (m_uCount == 0) m_uCount = 1;

				const auto m_uTotal = m_uIndex + m_uCount;
				if (m_uTotal >= max_length) return;

				if (max_length > (m_uTotal + 1)) for (auto u = m_uIndex; (static_cast<unsigned int>(max_length) - m_uTotal) >= u; ++u) operator[](u) = operator[](u + m_uCount);

				max_length -= m_uCount;
			}

			auto RemoveAll() -> void {
				if (max_length > 0) {
					memset(GetData(), 0, sizeof(Type) * max_length);
					max_length = 0;
				}
			}

			auto ToVector() -> std::vector<T> {
				std::vector<T> rs{};
				rs.reserve(this->max_length);
				for (auto i = 0; i < this->max_length; i++) rs.push_back(this->At(i));
				return rs;
			}

			static auto New(const Class* kalss, const std::uintptr_t size) -> Array* {
				if (mode_ == Mode::Il2Cpp) return UnityResolve::Invoke<Array*, void*, std::uintptr_t>("il2cpp_array_new", kalss->classinfo, size);
				return UnityResolve::Invoke<Array*, void*, void*, std::uintptr_t>("mono_array_new", pDomain, kalss->classinfo, size);
			}
		};

		template <typename Type>
		struct List : Object {
			Array<Type>* pList;
			int size{};
			int version{};
			void* syncRoot{};

			auto ToArray() -> Array<Type>* { return pList; }

			static auto New(const Class* kalss, const std::uintptr_t size) -> List* {
				auto pList = new List<Type>();
				pList->pList = Array<Type>::New(kalss, size);
				pList->size = size;
			}

			auto operator[](const unsigned int m_uIndex) -> Type& { return pList->At(m_uIndex); }

			auto Add(Type* pDate) -> float {
				static Method* method;
				if (!method) method = Get("mscorlib.dll")->Get("List")->Get<Method>("Add");
				if (method) return method->Invoke<void>(this, pDate);
				throw std::logic_error("nullptr");
			}

			auto Remove(Type* pDate) -> float {
				static Method* method;
				if (!method) method = Get("mscorlib.dll")->Get("List")->Get<Method>("Remove");
				if (method) return method->Invoke<void>(this, pDate);
				throw std::logic_error("nullptr");
			}

			auto RemoveAt(int index) -> float {
				static Method* method;
				if (!method) method = Get("mscorlib.dll")->Get("List")->Get<Method>("RemoveAt");
				if (method) return method->Invoke<void>(this, index);
				throw std::logic_error("nullptr");
			}

			auto ForEach(void(*action)(Type* pDate)) -> float {
				static Method* method;
				if (!method) method = Get("mscorlib.dll")->Get("List")->Get<Method>("ForEach");
				if (method) return method->Invoke<void>(this, action);
				throw std::logic_error("nullptr");
			}

			auto GetRange(int index, int count) -> float {
				static Method* method;
				if (!method) method = Get("mscorlib.dll")->Get("List")->Get<Method>("GetRange");
				if (method) return method->Invoke<void>(this, index, count);
				throw std::logic_error("nullptr");
			}

			auto Clear() -> float {
				static Method* method;
				if (!method) method = Get("mscorlib.dll")->Get("List")->Get<Method>("Clear");
				if (method) return method->Invoke<void>(this);
				throw std::logic_error("nullptr");
			}

			auto Sort(int(*comparison)(Type* pX, Type* pY)) -> float {
				static Method* method;
				if (!method) method = Get("mscorlib.dll")->Get("List")->Get<Method>("Sort");
				if (method) return method->Invoke<void>(this, comparison);
				throw std::logic_error("nullptr");
			}
		};

		template <typename TKey, typename TValue>
		struct Dictionary : Object {
			struct Entry {
				int    iHashCode;
				int    iNext;
				TKey   tKey;
				TValue tValue;
			};

			Array<int>* pBuckets;
			Array<Entry*>* pEntries;
			int            iCount;
			int            iVersion;
			int            iFreeList;
			int            iFreeCount;
			void* pComparer;
			void* pKeys;
			void* pValues;

			auto GetEntry() -> Entry* { return static_cast<Entry*>(pEntries->GetData()); }

			auto GetKeyByIndex(const int iIndex) -> TKey {
				TKey tKey = { 0 };

				Entry* pEntry = GetEntry();
				if (pEntry) tKey = pEntry[iIndex].m_tKey;

				return tKey;
			}

			auto GetValueByIndex(const int iIndex) -> TValue {
				TValue tValue = { 0 };

				Entry* pEntry = GetEntry();
				if (pEntry) tValue = pEntry[iIndex].m_tValue;

				return tValue;
			}

			auto GetValueByKey(const TKey tKey) -> TValue {
				TValue tValue = { 0 };
				for (auto i = 0; i < iCount; i++) if (GetEntry()[i].m_tKey == tKey) tValue = GetEntry()[i].m_tValue;
				return tValue;
			}

			auto operator[](const TKey tKey) const -> TValue { return GetValueByKey(tKey); }
		};

		struct UnityObject : Object {
			void* m_CachedPtr;

			auto GetName() -> std::string {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Object")->Get<Method>("get_name");
				if (method) return method->Invoke<String*>(this)->ToString();
				throw std::logic_error("nullptr");
			}

			auto ToString() -> std::string {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Object")->Get<Method>("ToString");
				if (method) return method->Invoke<String*>(this)->ToString();
				throw std::logic_error("nullptr");
			}

			static auto ToString(UnityObject* obj) -> std::string {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Object")->Get<Method>("ToString", { "*" });
				if (method) return method->Invoke<String*>(obj)->ToString();
				throw std::logic_error("nullptr");
			}

			static auto Instantiate(UnityObject* original) -> UnityObject* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Object")->Get<Method>("Instantiate", { "*" });
				if (method) return method->Invoke<UnityObject*>(original);
				throw std::logic_error("nullptr");
			}

			static auto Destroy(UnityObject* original) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Object")->Get<Method>("Destroy", { "*" });
				if (method) return method->Invoke<void>(original);
				throw std::logic_error("nullptr");
			}
		};

		struct Component : UnityObject {
			auto GetTransform() -> Transform* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Component")->Get<Method>("get_transform");
				if (method) return method->Invoke<Transform*>(this);
				throw std::logic_error("nullptr");
			}

			auto GetGameObject() -> GameObject* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Component")->Get<Method>("get_gameObject");
				if (method) return method->Invoke<GameObject*>(this);
				throw std::logic_error("nullptr");
			}

			auto GetTag() -> std::string {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Component")->Get<Method>("get_tag");
				if (method) return method->Invoke<String*>(this)->ToString();
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponentsInChildren() -> Array<T>* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Component")->Get<Method>("GetComponentsInChildren");
				if (method) return method->Invoke<Array<T>*>(this);
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponentsInChildren(Class* pClass) -> Array<T>* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Component")->Get<Method>("GetComponentsInChildren", { "System.Type" });;
				if (method) return method->Invoke<Array<T>*>(this, pClass->GetType().GetObject());
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponents() -> Array<T>* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Component")->Get<Method>("GetComponents");
				if (method) return method->Invoke<Array<T>*>(this);
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponents(Class* pClass) -> Array<T>* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Component")->Get<Method>("GetComponents", { "System.Type" });;
				if (method) return method->Invoke<Array<T>*>(this, pClass->GetType().GetObject());
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponentsInParent() -> Array<T>* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Component")->Get<Method>("GetComponentsInParent");
				if (method) return method->Invoke<Array<T>*>(this);
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponentsInParent(Class* pClass) -> Array<T>* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Component")->Get<Method>("GetComponentsInParent", { "System.Type" });;
				if (method) return method->Invoke<Array<T>*>(this, pClass->GetType().GetObject());
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponentInChildren(Class* pClass) -> T {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Component")->Get<Method>("GetComponentInChildren", { "System.Type" });;
				if (method) return method->Invoke<T>(this, pClass->GetType().GetObject());
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponentInParent(Class* pClass) -> T {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Component")->Get<Method>("GetComponentInParent", { "System.Type" });;
				if (method) return method->Invoke<T>(this, pClass->GetType().GetObject());
				throw std::logic_error("nullptr");
			}
		};

		struct Camera : Component {
			enum class Eye : int {
				Left,
				Right,
				Mono
			};

			static auto GetMain() -> Camera* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Camera")->Get<Method>("get_main");
				if (method) return method->Invoke<Camera*>();
				throw std::logic_error("nullptr");
			}

			static auto GetCurrent() -> Camera* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Camera")->Get<Method>("get_current");
				if (method) return method->Invoke<Camera*>();
				throw std::logic_error("nullptr");
			}

			static auto GetAllCount() -> int {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Camera")->Get<Method>("get_allCamerasCount");
				if (method) return method->Invoke<int>();
				throw std::logic_error("nullptr");
			}

			static auto GetAllCamera() -> std::vector<Camera*> {
				static Method* method;
				static Class* klass;

				if (!method || !klass) {
					method = Get("UnityEngine.CoreModule.dll")->Get("Camera")->Get<Method>("GetAllCameras", { "*" });
					klass = Get("UnityEngine.CoreModule.dll")->Get("Camera");
				}

				if (method && klass) {
					const auto array = Array<Camera*>::New(klass, GetAllCount());
					method->Invoke<int>(array);
					return array->ToVector();
				}

				throw std::logic_error("nullptr");
			}

			auto GetDepth() -> float {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Camera")->Get<Method>("get_depth");
				if (method) return method->Invoke<float>(this);
				throw std::logic_error("nullptr");
			}

			auto SetDepth(const float depth) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Camera")->Get<Method>("set_depth", { "*" });
				if (method) return method->Invoke<void>(this, depth);
			}

			auto WorldToScreenPoint(const Vector3& position, const Eye eye) -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Camera")->Get<Method>(mode_ == Mode::Mono ? "WorldToScreenPoint_Injected" : "WorldToScreenPoint");
				if (mode_ == Mode::Mono && method) {
					const Vector3 vec3{};
					method->Invoke<void>(this, position, eye, &vec3);
					return vec3;
				}
				if (method) return method->Invoke<Vector3>(this, position, eye);
				throw std::logic_error("nullptr");
			}

			auto ScreenToWorldPoint(const Vector3& position, const Eye eye) -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Camera")->Get<Method>(mode_ == Mode::Mono ? "ScreenToWorldPoint_Injected" : "ScreenToWorldPoint");
				if (mode_ == Mode::Mono && method) {
					const Vector3 vec3{};
					method->Invoke<void>(this, position, eye, &vec3);
					return vec3;
				}
				if (method) return method->Invoke<Vector3>(this, position, eye);
				throw std::logic_error("nullptr");
			}

			auto CameraToWorldMatrix() -> Matrix4x4 {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Camera")->Get<Method>(mode_ == Mode::Mono ? "get_cameraToWorldMatrix_Injected" : "get_cameraToWorldMatrix");
				if (mode_ == Mode::Mono && method) {
					Matrix4x4 matrix4{};
					method->Invoke<void>(this, &matrix4);
					return matrix4;
				}
				if (method) return method->Invoke<Matrix4x4>(this);
				throw std::logic_error("nullptr");
			}
		};

		struct Transform : Component {
			auto GetPosition() -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>(mode_ == Mode::Mono ? "get_position_Injected" : "get_position");
				if (mode_ == Mode::Mono && method) {
					const Vector3 vec3{};
					method->Invoke<void>(this, &vec3);
					return vec3;
				}
				if (method) return method->Invoke<Vector3>(this);
				return {};
			}

			auto SetPosition(const Vector3& position) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>(mode_ == Mode::Mono ? "set_position_Injected" : "set_position");
				if (mode_ == Mode::Mono && method) return method->Invoke<void>(this, &position);
				if (method) return method->Invoke<void>(this, position);
				throw std::logic_error("nullptr");
			}

			auto GetRotation() -> Quaternion {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>(mode_ == Mode::Mono ? "get_rotation_Injected" : "get_rotation");
				if (mode_ == Mode::Mono && method) {
					const Quaternion vec3{};
					method->Invoke<void>(this, &vec3);
					return vec3;
				}
				if (method) return method->Invoke<Quaternion>(this);
				return {};
			}

			auto SetRotation(const Quaternion& position) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>(mode_ == Mode::Mono ? "set_rotation_Injected" : "set_rotation");
				if (mode_ == Mode::Mono && method) return method->Invoke<void>(this, &position);
				if (method) return method->Invoke<void>(this, position);
				throw std::logic_error("nullptr");
			}

			auto GetLocalPosition() -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>(mode_ == Mode::Mono ? "get_localPosition_Injected" : "get_localPosition");
				if (mode_ == Mode::Mono && method) {
					const Vector3 vec3{};
					method->Invoke<void>(this, &vec3);
					return vec3;
				}
				if (method) return method->Invoke<Vector3>(this);
				return {};
			}

			auto SetLocalPosition(const Vector3& position) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>(mode_ == Mode::Mono ? "set_localPosition_Injected" : "set_localPosition");
				if (mode_ == Mode::Mono && method) return method->Invoke<void>(this, &position);
				if (method) return method->Invoke<void>(this, position);
				throw std::logic_error("nullptr");
			}

			auto GetLocalRotation() -> Quaternion {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>(mode_ == Mode::Mono ? "get_localRotation_Injected" : "get_localRotation");
				if (mode_ == Mode::Mono && method) {
					const Quaternion vec3{};
					method->Invoke<void>(this, &vec3);
					return vec3;
				}
				if (method) return method->Invoke<Quaternion>(this);
				return {};
			}

			auto SetLocalRotation(const Quaternion& position) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>(mode_ == Mode::Mono ? "set_localRotation_Injected" : "set_localRotation");
				if (mode_ == Mode::Mono && method) return method->Invoke<void>(this, &position);
				if (method) return method->Invoke<void>(this, position);
				throw std::logic_error("nullptr");
			}

			auto GetLocalScale() -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>(mode_ == Mode::Mono ? "get_localScale_Injected" : "get_localScale");
				if (mode_ == Mode::Mono && method) {
					const Vector3 vec3{};
					method->Invoke<void>(this, &vec3);
					return vec3;
				}
				if (method) return method->Invoke<Vector3>(this);
				return {};
			}

			auto SetLocalScale(const Vector3& position) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>(mode_ == Mode::Mono ? "set_localScale_Injected" : "set_localScale");
				if (mode_ == Mode::Mono && method) return method->Invoke<void>(this, &position);
				if (method) return method->Invoke<void>(this, position);
				throw std::logic_error("nullptr");
			}

			auto GetChildCount() -> int {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>("get_childCount");
				if (method) return method->Invoke<int>(this);
				throw std::logic_error("nullptr");
			}

			auto GetChild(const int index) -> Transform* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>("GetChild");
				if (method) return method->Invoke<Transform*>(this, index);
				throw std::logic_error("nullptr");
			}

			auto GetRoot() -> Transform* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>("GetRoot");
				if (method) return method->Invoke<Transform*>(this);
				throw std::logic_error("nullptr");
			}

			auto GetParent() -> Transform* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>("GetParent");
				if (method) return method->Invoke<Transform*>(this);
				throw std::logic_error("nullptr");
			}

			auto GetLossyScale() -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>(mode_ == Mode::Mono ? "get_lossyScale_Injected" : "get_lossyScale");
				if (mode_ == Mode::Mono && method) {
					const Vector3 vec3{};
					method->Invoke<void>(this, &vec3);
					return vec3;
				}
				if (method) return method->Invoke<Vector3>(this);
				return {};
			}

			auto TransformPoint(const Vector3& position) -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>(mode_ == Mode::Mono ? "TransformPoint_Injected" : "TransformPoint");
				if (mode_ == Mode::Mono && method) {
					const Vector3 vec3{};
					method->Invoke<void>(this, position, &vec3);
					return vec3;
				}
				if (method) return method->Invoke<Vector3>(this, position);
				return {};
			}

			auto LookAt(const Vector3& worldPosition) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>("LookAt", { "Vector3" });
				if (method) return method->Invoke<void>(this, worldPosition);
				throw std::logic_error("nullptr");
			}

			auto Rotate(const Vector3& eulers) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Transform")->Get<Method>("Rotate", { "Vector3" });
				if (method) return method->Invoke<void>(this, eulers);
				throw std::logic_error("nullptr");
			}
		};

		struct GameObject : UnityObject {
			static auto Create(GameObject* obj, const std::string& name) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("GameObject")->Get<Method>("Internal_CreateGameObject");
				if (method) method->Invoke<void, GameObject*, String*>(obj, String::New(name));
				throw std::logic_error("nullptr");
			}

			static auto FindGameObjectsWithTag(const std::string& name) -> std::vector<GameObject*> {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("GameObject")->Get<Method>("FindGameObjectsWithTag");
				if (method) {
					std::vector<GameObject*> rs{};
					const auto               array = method->Invoke<Array<GameObject*>*>(String::New(name));
					rs.reserve(array->max_length);
					for (auto i = 0; i < array->max_length; i++) rs.push_back(array->At(i));
					return rs;
				}
				throw std::logic_error("nullptr");
			}

			static auto Find(const std::string& name) -> GameObject* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("GameObject")->Get<Method>("Find");
				if (method) return method->Invoke<GameObject*>(String::New(name));
				throw std::logic_error("nullptr");
			}

			auto GetTransform() -> Transform* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("GameObject")->Get<Method>("get_transform");
				if (method) return method->Invoke<Transform*>(this);
				throw std::logic_error("nullptr");
			}

			auto GetIsStatic() -> bool {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("GameObject")->Get<Method>("get_isStatic");
				if (method) return method->Invoke<bool>(this);
				throw std::logic_error("nullptr");
			}

			auto GetTag() -> String* {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("GameObject")->Get<Method>("get_tag");
				if (method) return method->Invoke<String*>(this);
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponent() -> T {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("GameObject")->Get<Method>("GetComponent");
				if (method) return method->Invoke<T>(this);
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponent(const Class* type) -> T {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("GameObject")->Get<Method>("GetComponent", { "System.Type" });
				if (method) return method->Invoke<T>(this, type->GetType().GetObject());
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponentInChildren(const Class* type) -> T {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("GameObject")->Get<Method>("GetComponentInChildren", { "System.Type" });
				if (method) return method->Invoke<T>(this, type->GetType().GetObject());
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponentInParent(const Class* type) -> T {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("GameObject")->Get<Method>("GetComponentInParent", { "System.Type" });
				if (method) return method->Invoke<T>(this, type->GetType().GetObject());
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponents(Class* type, bool useSearchTypeAsArrayReturnType = false, bool recursive = false, bool includeInactive = true, bool reverse = false, List<T>* resultList = nullptr) -> std::vector<T> {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("GameObject")->Get<Method>("GetComponentsInternal");
				if (method) return method->Invoke<Array<T>*>(this, type->GetType().GetObject(), useSearchTypeAsArrayReturnType, recursive, includeInactive, reverse, resultList)->ToVector();
				throw std::logic_error("nullptr");
			}

			template <typename T>
			auto GetComponentsInChildren(Class* type, const bool includeInactive = false) -> std::vector<T> { return GetComponents<T>(type, false, true, includeInactive, false, nullptr); }


			template <typename T>
			auto GetComponentsInParent(Class* type, const bool includeInactive = false) -> std::vector<T> { return GetComponents<T>(type, false, true, includeInactive, true, nullptr); }
		};

		struct LayerMask : Object {
			int m_Mask;

			static auto NameToLayer(const std::string& layerName) -> int {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("LayerMask")->Get<Method>("NameToLayer");
				if (method) return method->Invoke<int>(String::New(layerName));
				throw std::logic_error("nullptr");
			}

			static auto LayerToName(const int layer) -> std::string {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("LayerMask")->Get<Method>("LayerToName");
				if (method) return method->Invoke<String*>(layer)->ToString();
				throw std::logic_error("nullptr");
			}
		};

		struct Rigidbody : Component {
			auto GetDetectCollisions() -> bool {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("Rigidbody")->Get<Method>("get_detectCollisions");
				if (method) return method->Invoke<bool>(this);
				throw std::logic_error("nullptr");
			}

			auto SetDetectCollisions(const bool value) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("Rigidbody")->Get<Method>("set_detectCollisions");
				if (method) return method->Invoke<void>(this, value);
				throw std::logic_error("nullptr");
			}

			auto GetVelocity() -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("Rigidbody")->Get<Method>(mode_ == Mode::Mono ? "get_velocity_Injected" : "get_velocity");
				if (mode_ == Mode::Mono && method) {
					Vector3 vector;
					method->Invoke<void>(this, &vector);
					return vector;
				}
				if (method) return method->Invoke<Vector3>(this);
				throw std::logic_error("nullptr");
			}

			auto SetVelocity(Vector3 value) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("Rigidbody")->Get<Method>(mode_ == Mode::Mono ? "set_velocity_Injected" : "set_velocity");
				if (mode_ == Mode::Mono && method) return method->Invoke<void>(this, &value);
				if (method) return method->Invoke<void>(this, value);
				throw std::logic_error("nullptr");
			}
		};

		struct Collider : Component {
			auto GetBounds() -> Bounds {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("Collider")->Get<Method>("get_bounds_Injected");
				if (method) {
					Bounds bounds;
					method->Invoke<void>(this, &bounds);
					return bounds;
				}
				throw std::logic_error("nullptr");
			}
		};

		struct Mesh : UnityObject {
			auto GetBounds() -> Bounds {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Mesh")->Get<Method>("get_bounds_Injected");
				if (method) {
					Bounds bounds;
					method->Invoke<void>(this, &bounds);
					return bounds;
				}
				throw std::logic_error("nullptr");
			}
		};

		struct CapsuleCollider : Collider {
			auto GetCenter() -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("CapsuleCollider")->Get<Method>("get_center");
				if (method) return method->Invoke<Vector3>(this);
				throw std::logic_error("nullptr");
			}

			auto GetDirection() -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("CapsuleCollider")->Get<Method>("get_direction");
				if (method) return method->Invoke<Vector3>(this);
				throw std::logic_error("nullptr");
			}

			auto GetHeightn() -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("CapsuleCollider")->Get<Method>("get_height");
				if (method) return method->Invoke<Vector3>(this);
				throw std::logic_error("nullptr");
			}

			auto GetRadius() -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("CapsuleCollider")->Get<Method>("get_radius");
				if (method) return method->Invoke<Vector3>(this);
				throw std::logic_error("nullptr");
			}
		};

		struct BoxCollider : Collider {
			auto GetCenter() -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("BoxCollider")->Get<Method>("get_center");
				if (method) return method->Invoke<Vector3>(this);
				throw std::logic_error("nullptr");
			}

			auto GetSize() -> Vector3 {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("BoxCollider")->Get<Method>("get_size");
				if (method) return method->Invoke<Vector3>(this);
				throw std::logic_error("nullptr");
			}
		};

		struct Renderer : Component {
			auto GetBounds() -> Bounds {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Renderer")->Get<Method>("get_bounds_Injected");
				if (method) {
					Bounds bounds;
					method->Invoke<void>(this, &bounds);
					return bounds;
				}
				throw std::logic_error("nullptr");
			}
		};

		struct Behaviour : Component {
			auto GetEnabled() -> bool {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Behaviour")->Get<Method>("get_enabled");
				if (method) return method->Invoke<bool>(this);
				throw std::logic_error("nullptr");
			}

			auto SetEnabled(const bool value) -> bool {
				static Method* method;
				if (!method) method = Get("UnityEngine.CoreModule.dll")->Get("Behaviour")->Get<Method>("set_enabled");
				if (method) return method->Invoke<bool>(this, value);
				throw std::logic_error("nullptr");
			}
		};

		struct MonoBehaviour : Behaviour {
		};

		struct Physics : Object {
			static auto Linecast(const Vector3& start, const Vector3& end) -> bool {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("Physics")->Get<Method>("Linecast", { "*", "*" });
				if (method) return method->Invoke<bool>(start, end);
				throw std::logic_error("nullptr");
			}

			static auto Raycast(const Vector3& origin, const Vector3& direction, const float maxDistance) -> bool {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("Physics")->Get<Method>("Raycast", { "*", "*", "*" });
				if (method) return method->Invoke<bool>(origin, direction, maxDistance);
				throw std::logic_error("nullptr");
			}

			static auto IgnoreCollision(Collider* collider1, Collider* collider2) -> void {
				static Method* method;
				if (!method) method = Get("UnityEngine.PhysicsModule.dll")->Get("Physics")->Get<Method>("IgnoreCollision1", { "*", "*" });
				if (method) return method->Invoke<void>(collider1, collider2);
				throw std::logic_error("nullptr");
			}
		};

		struct Animator : Behaviour {
			enum class HumanBodyBones : int {
				Hips,
				LeftUpperLeg,
				RightUpperLeg,
				LeftLowerLeg,
				RightLowerLeg,
				LeftFoot,
				RightFoot,
				Spine,
				Chest,
				UpperChest = 54,
				Neck = 9,
				Head,
				LeftShoulder,
				RightShoulder,
				LeftUpperArm,
				RightUpperArm,
				LeftLowerArm,
				RightLowerArm,
				LeftHand,
				RightHand,
				LeftToes,
				RightToes,
				LeftEye,
				RightEye,
				Jaw,
				LeftThumbProximal,
				LeftThumbIntermediate,
				LeftThumbDistal,
				LeftIndexProximal,
				LeftIndexIntermediate,
				LeftIndexDistal,
				LeftMiddleProximal,
				LeftMiddleIntermediate,
				LeftMiddleDistal,
				LeftRingProximal,
				LeftRingIntermediate,
				LeftRingDistal,
				LeftLittleProximal,
				LeftLittleIntermediate,
				LeftLittleDistal,
				RightThumbProximal,
				RightThumbIntermediate,
				RightThumbDistal,
				RightIndexProximal,
				RightIndexIntermediate,
				RightIndexDistal,
				RightMiddleProximal,
				RightMiddleIntermediate,
				RightMiddleDistal,
				RightRingProximal,
				RightRingIntermediate,
				RightRingDistal,
				RightLittleProximal,
				RightLittleIntermediate,
				RightLittleDistal,
				LastBone = 55
			};

			auto GetBoneTransform(const HumanBodyBones humanBoneId) -> Transform* {
				static Method* method;
				if (!method) method = Get("UnityEngine.AnimationModule.dll")->Get("Animator")->Get<Method>("GetBoneTransform");
				if (method) return method->Invoke<Transform*>(this, humanBoneId);
				throw std::logic_error("nullptr");
			}
		};

		template <typename Return, typename... Args>
		static auto Invoke(const void* address, Args... args) -> Return {
#if WINDOWS_MODE
			static bool badPtr;
			try {
				if (!badPtr) badPtr = !IsBadCodePtr(static_cast<FARPROC>(function));
				if (address != nullptr && badPtr) return reinterpret_cast<Return(*)(Args...)>(address)(args...);
			}
			catch (...) {}
#elif
			try {
				if (address != nullptr) return reinterpret_cast<Return(*)(Args...)>(address)(args...);
			}
			catch (...) {}
#endif
			return Return();
		}
	};

private:
	inline static Mode                                   mode_{};
	inline static void* hmodule_;
	inline static std::unordered_map<std::string, void*> address_{};
	inline static void* pDomain{};
};
#endif // UNITYRESOLVE_HPP