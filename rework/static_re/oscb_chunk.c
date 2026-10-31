class OSMetaClassBase
{
public:


/*!
 * @define OSTypeAlloc
 * @hidecontents
 *
 * @abstract
 * Allocates an instance of the named object class.
 *
 * @param type    The name of the desired class to be created,
 *                as a raw token, <i>not</i> a string or macro.
 *
 * @result
 * A pointer to the new, uninitialized object on success;
 * <code>NULL</code> on failure.
 *
 * @discussion
 * See also
 * <code>@link
 * //apple_ref/cpp/clm/OSMetaClass/allocClassWithName/staticOSObject*\/(constchar*)
 * OSMetaClass::allocClassWithName(const char *)@/link</code>
 * and
 * <code>@link
 * //apple_ref/cpp/instm/OSMetaClass/alloc/virtualOSObject*\/()
 * OSMetaClass::alloc@/link</code>.
 *
 * The OSTypeAlloc macro is used to avoid binary compatibility difficulties
 * presented by the C++ <code>new</code> operator.
 */
#define OSTypeAlloc(type)   ((type *) ((type::metaClass)->alloc()))


/*!
 * @define OSTypeID
 * @hidecontents
 *
 * @abstract
 * Returns the type ID (metaclass) of a class based on its name.
 *
 * @param type    The name of the desired class, as a raw token,
 *                <i>not</i> a string or macro.
 *
 * @result
 * The unique type ID (metaclass) for the class.
 *
 * @discussion
 * It is typically more useful to determine whether a class is derived
 * from another; see
 * <code>@link //apple_ref/cpp/macro/OSDynamicCast OSDynamicCast@/link</code>
 * and
 * <code>@link //apple_ref/cpp/macro/OSCheckTypeInst OSCheckTypeInst@/link</code>.
 */
#define OSTypeID(type)   (type::metaClass)
#define OSMTypeID(type)  (const_cast<OSMetaClass *>(type::metaClass))


/*!
 * @define OSTypeIDInst
 * @hidecontents
 *
 * @abstract
 * Returns the type ID (metaclass) for the class of an object instance.
 *
 * @param typeinst An instance of an OSObject subclass.
 *
 * @result
 * The type ID of that object's class; that is, its metaclass.
 *
 * @discussion
 * It is typically more useful to determine whether an object is derived
 * from a particular class; see
 * <code>@link //apple_ref/cpp/macro/OSDynamicCast OSDynamicCast@/link</code>
 * and
 * <code>@link //apple_ref/cpp/macro/OSCheckTypeInst OSCheckTypeInst@/link</code>.
 */
#define OSTypeIDInst(typeinst)   ((typeinst)->getMetaClass())


/*!
 * @define OSDynamicCast
 * @hidecontents
 *
 * @abstract
 * Safe type-casting for Libkern C++ objects.
 *
 * @param type    The name of the desired class type, as a raw token,
 *                <i>not</i> a string or macro.
 *                It is assumed you intend to cast to a pointer
 *                to an object of this type.
 *                Type qualifiers, such as <code>const</code>,
 *                are not recognized and will cause
 *                a (usually obscure) compile error.
 * @param inst    A pointer to the object instance to be cast.
 *                May be <code>NULL</code>.
 *
 * @result
 * <code>inst</code> if it is non-<code>NULL</code>
 * and derived from <code>type</code>;
 * otherwise <code>NULL</code>.
 *
 * @discussion
 * <code>OSDynamicCast</code> is a rough equivalent
 * to the standard C++ RTTI <code>dynamic_cast&lt;T&gt;</code> operator.
 * Your code should use this instead of raw C type-casting,
 * and check the resulting value.
 * If the result is non-<code>NULL</code>,
 * the object is safe to use as the type-cast class;
 * if the result is <code>NULL</code>,
 * the object does not derive from the type-cast class
 * and your code should take appropriate steps to handle the error.
 */
#define OSDynamicCast(type, inst)   \
    ((type *) OSMetaClassBase::safeMetaCast((inst), OSTypeID(type)))

/*!
 * @define OSRequiredCast
 * @hidecontents
 *
 * @abstract
 * Safe type-casting for Libkern C++ objects; panics on failure.
 * The input parameters are the same as for the {@code OSDynamicCast} macro.
 *
 * @result {@code inst} if it is NULL or derived from {@code type};
 * otherwise triggers a kernel panic.
 *
 * @discussion
 * This macro should be used in place of C-style casts or
 * <code>@link OSDynamicCast OSDynamicCast@/link</code>.
 * when the caller is absolutely sure that the passed
 * argument is a subclass of a required type.
 * It is equivalent to using {@code OSDynamicCast} and crashing with a kernel
 * panic on cast failure.
 */
#define OSRequiredCast(type, inst)  \
    ((type *) OSMetaClassBase::requiredMetaCast((inst), OSTypeID(type)))

/*!
 * @define OSCheckTypeInst
 * @hidecontents
 *
 * @abstract
 * Checks whether two objects are type-compatible.
 *
 * @param typeinst The reference object.
 * @param inst     The object to check for type compatibility.
 *
 * @result
 * <code>true</code> if both <code>inst</code> and
 * <code>typeinst</code> are non-<code>NULL</code>
 * and <code>inst</code> is derived from the class of <code>typeinst</code>;
 * otherwise <code>false</code>.
 */
#define OSCheckTypeInst(typeinst, inst) \
    OSMetaClassBase::checkTypeInst(inst, typeinst)

#define OSSafeRelease(inst) \
  do { int OSSafeRelease __attribute__ ((deprecated("Use OSSafeReleaseNULL"))); (OSSafeRelease); \
	if (inst) (inst)->release(); } while (0)

/*! @function OSSafeReleaseNULL
 *  @abstract Release an object if not <code>NULL</code>, then set it to <code>NULL</code>.
 *  @param    inst  Instance of an OSObject, may be <code>NULL</code>.
 */
#define OSSafeReleaseNULL(inst)   do { if (inst != NULL) (inst)->release(); (inst) = NULL; } while (0)

	typedef void (*_ptf_t)(void);

#if defined(__arm__) || defined(__arm64__)

	static _ptf_t _ptmf2ptf(const OSMetaClassBase * self, void (OSMetaClassBase::*func)(void));

#elif defined(__i386__) || defined(__x86_64__)

// Slightly less arcane and slightly less evil code to do
// the same for kexts compiled with the standard Itanium C++
// ABI

	static inline _ptf_t
	_ptmf2ptf(const OSMetaClassBase *self, void (OSMetaClassBase::*func)(void))
	{
		union {
			void (OSMetaClassBase::*fIn)(void);
			uintptr_t fVTOffset;
			_ptf_t fPFN;
		} map;

		map.fIn = func;

		if (map.fVTOffset & 1) {
			// virtual
			union {
				const OSMetaClassBase *fObj;
				_ptf_t **vtablep;
			} u;
			u.fObj = self;

			// Virtual member function so dereference vtable
			return *(_ptf_t *)(((uintptr_t)*u.vtablep) + map.fVTOffset - 1);
		} else {
			// Not virtual, i.e. plain member func
			return map.fPFN;
		}
	}

#else
#error Unknown architecture.
#endif /* __arm__ */


/*!
 * @define OSMemberFunctionCast
 * @hidecontents
 *
 * @abstract
 * Converts a C++ member function pointer, relative to an instance,
 * to a C-style pointer to function.
 *
 * @param cptrtype The function type declaration to cast to
 *                 (typically provided as a <code>typedef</code> by  I/O KitKit classes).
 * @param self     The <code>this</code> pointer of the object whose function
 *                 you wish to cache.
 * @param func     The pointer to the member function itself,
 *                 something like <code>&Class::function</code>.
 *                 It should be an explicit member function pointer constant,
 *                 rather than a variable.
 *                 Don't pass a <code>NULL</code> member function pointer.
 *                 Instead, directly use a <code>NULL</code> function pointer.
 *
 * @result
 * A pointer to a function of the given type referencing <code>self</code>.
 *
 * @discussion
 * This function is used to generate pointers to C++ functions for instances,
 * such that they can be registered as callbacks with I/O Kit objects.
 *
 * No warnings are generated.
 *
 * This function will panic if an attempt is made to call it
 * with a multiply-inheriting class.
 */
#if __has_builtin(__builtin_load_member_function_pointer)
#define OSMemberFunctionCast(cptrtype, self, func)                       \
     ((cptrtype) __builtin_load_member_function_pointer(*self, func) ? : \
      (cptrtype) OSMetaClassBase::                                       \
	  _ptmf2ptf(self, (void (OSMetaClassBase::*)(void)) func))
#else
#define OSMemberFunctionCast(cptrtype, self, func)         \
    (cptrtype) OSMetaClassBase::                           \
	_ptmf2ptf(self, (void (OSMetaClassBase::*)(void)) func)
#endif

protected:
	OSMetaClassBase();
	virtual
	~OSMetaClassBase();

private:
// Disable copy constructors of OSMetaClassBase based objects
/* Not to be included in headerdoc.
 *
 * @function operator =
 *
 * @abstract
 * Disable implicit copy constructor by making private
 *
 * @param src Reference to source object that isn't allowed to be copied.
 */
	void operator =(OSMetaClassBase &src);

/* Not to be included in headerdoc.
 *
 * @function OSMetaClassBase
 *
 * @abstract
 * Disable implicit copy constructor by making private
 *
 * @param src Reference to source object that isn't allowed to be copied.
 */
	OSMetaClassBase(OSMetaClassBase &src);

public:

// xx-review: the original comment for this makes it sound to me like we don't
// xx-review: catch over-releasing an object...?

/*!
 * @function release
 *
 * @abstract
 * Abstract declaration of
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/release/virtualvoid/(int)
 * release(int freeWhen)@/link</code>.
 *
 * @discussion
 * See
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/release/virtualvoid/(int)
 * release(int freeWhen)@/link</code>.
 */
	virtual void release(int freeWhen) const = 0;


/*!
 * @function getRetainCount
 *
 * @abstract
 * Abstract declaration of
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/getRetainCount/virtualint/()
 * getRetainCount()@/link</code>.
 *
 * @discussion
 * See
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/getRetainCount/virtualint/()
 * OSObject::getRetainCount()@/link</code>.
 */
	virtual int getRetainCount() const = 0;


/*!
 * @function retain
 *
 * @abstract
 * Abstract declaration of
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/retain/virtualvoid/()
 * retain()@/link</code>.
 *
 * @discussion
 * See
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/retain/virtualvoid/()
 * OSObject::retain()@/link</code>.
 */
	virtual void retain() const = 0;


/*!
 * @function release
 *
 * @abstract
 * Abstract declaration of
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/release/virtualvoid/()
 * release@/link</code>.
 *
 * @discussion
 * See
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/release/virtualvoid/()
 * OSObject::release@/link</code>.
 */
	virtual void release() const = 0;


/*!
 * @function serialize
 *
 * @abstract
 * Abstract declaration of
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/serialize/virtualbool/(OSSerialize*)
 * serialize@/link</code>.
 *
 * @discussion
 * See
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/serialize/virtualbool/(OSSerialize*)
 * OSObject::serialize@/link</code>.
 */
	virtual bool serialize(OSSerialize * serializer) const = 0;


/*!
 * @function getMetaClass
 *
 * @abstract
 * Returns the OSMetaClass representing
 * an OSMetaClassBase subclass.
 *
 * @discussion
 * OSObject overrides this abstract member function
 * to return the OSMetaClass object that represents
 * each class for run-time typing.
 */
	virtual const OSMetaClass * getMetaClass() const = 0;


/*!
 * @function isEqualTo
 *
 * @abstract
 * Checks whether another object is equal to the receiver.
 *
 * @param anObject The object to copmare to the receiver.
 *
 * @result
 * <code>true</code> if the objects are equal, <code>false</code> otherwise.
 *
 * @discussion
 * OSMetaClassBase implements this as a direct pointer comparison,
 * since it has no other information to judge equality by.
 * Subclasses generally override this function
 * to do a more meaningful comparison.
 * For example, OSString implements it to return
 * <code>true</code> if <code>anObject</code>
 * is derived from OSString and represents the same C string.
 */
	virtual bool isEqualTo(const OSMetaClassBase * anObject) const;


/*!
 * @function metaCast
 *
 * @abstract
 * Casts this object is to the class managed by the given OSMetaClass.
 *
 * @param toMeta A pointer to a constant OSMetaClass
 *               for the desired target type.
 *
 * @result
 * <code>this</code> if the object is derived
 * from the class managed by <code>toMeta</code>,
 * otherwise <code>NULL</code>.
 *
 * @discussion
 * It is far more convenient to use
 * <code>@link OSDynamicCast OSDynamicCast@/link</code>.
 */
	OSMetaClassBase * metaCast(const OSMetaClass * toMeta) const;


/*!
 * @function metaCast
 *
 * @abstract
 * Casts this object is to the class managed by the named OSMetaClass.
 *
 * @param toMeta An OSSymbol naming the desired target type.
 *
 * @result
 * <code>this</code> if the object is derived
 * from the class named by <code>toMeta</code>,
 * otherwise <code>NULL</code>.
 *
 * @discussion
 * It is far more convenient to use
 * <code>@link OSDynamicCast OSDynamicCast@/link</code>.
 */
	OSMetaClassBase * metaCast(const OSSymbol * toMeta) const;


/*!
 * @function metaCast
 *
 * @abstract
 * Casts this object is to the class managed by the named OSMetaClass.
 *
 * @param toMeta An OSString naming the desired target type.
 * @result
 * <code>this</code> if the object is derived
 * from the class named by <code>toMeta</code>,
 * otherwise <code>NULL</code>.
 *
 * @discussion
 * It is far more convenient to use
 * <code>@link OSDynamicCast OSDynamicCast@/link</code>.
 */
	OSMetaClassBase * metaCast(const OSString * toMeta) const;


/*!
 * @function metaCast
 *
 * @abstract
 * Casts this object is to the class managed by the named OSMetaClass.
 *
 * @param toMeta A C string naming the desired target type.
 * @result
 * <code>this</code> if the object is derived
 * from the class named by <code>toMeta</code>,
 * otherwise <code>NULL</code>.
 *
 * @discussion
 * It is far more convenient to use
 * <code>@link OSDynamicCast OSDynamicCast@/link</code>.
 */
	OSMetaClassBase * metaCast(const char * toMeta) const;

// Helper inlines for run-time type preprocessor macros
/*!
 * @function safeMetaCast
 *
 * @abstract
 * Casts an object is to the class managed by the given OSMetaClass.
 *
 * @param anObject A pointer to the object to be cast.
 * @param toMeta   A pointer to a constant OSMetaClass
 *                 for the desired target type.
 *
 * @result
 * <code>anObject</code> if the object is derived
 * from the class managed by <code>toMeta</code>,
 * otherwise <code>NULL</code>.
 *
 * @discussion
 * It is far more convenient to use
 * <code>@link OSDynamicCast OSDynamicCast@/link</code>.
 */
	static OSMetaClassBase * safeMetaCast(
		const OSMetaClassBase * anObject,
		const OSMetaClass     * toMeta);

/*!
 * @function requiredMetaCast
 *
 * @abstract
 * Casts an object to the class managed by the given OSMetaClass or
 * fails with a kernel panic if the cast does not succeed.
 *
 * @param anObject A pointer to the object to be cast.
 * @param toMeta   A pointer to a constant OSMetaClass
 *                 for the desired target type.
 *
 * @result
 * <code>anObject</code> if the object is derived
 * from the class managed by <code>toMeta</code>,
 * <code>NULL</code> if <code>anObject</code> was <code>NULL</code>,
 * kernel panic otherwise.
 *
 * @discussion
 * It is far more convenient to use
 * <code>@link OSRequiredCast OSRequiredCast@/link</code>.
 */
	static OSMetaClassBase *requiredMetaCast(
		const OSMetaClassBase * anObject,
		const OSMetaClass     * toMeta);

/*!
 * @function checkTypeInst
 *
 * @abstract
 * Checks whether an object instance is of the same class
 * as another object instance (or a subclass of that class).
 *
 * @param inst       A pointer to the object to check.
 * @param typeinst   A pointer to an object of the class being checked.
 *
 * @result
 * <code>true</code> if the object is derived
 * from the class of <code>typeinst</code>
 * or a subclass of that class,
 * otherwise <code>false</code>.
 *
 * @discussion
 * It is far more convenient to use
 * <code>@link OSCheckTypeInst OSCheckTypeInst@/link</code>.
 */
	static bool checkTypeInst(
		const OSMetaClassBase * inst,
		const OSMetaClassBase * typeinst);

	static void initialize(void);

public:

/*!
 * @function taggedRetain
 *
 * @abstract
 * Abstract declaration of
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/taggedRetain/virtualvoid/(constvoid*)
 * taggedRetain(const void *)@/link</code>.
 *
 * @discussion
 * See
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/taggedRetain/virtualvoid/(constvoid*)
 * OSObject::taggedRetain(const void *)@/link</code>.
 */
// WAS: virtual void _RESERVEDOSMetaClassBase0();
	virtual void taggedRetain(const void * tag = NULL) const = 0;


/*!
 * @function taggedRelease
 *
 * @abstract
 * Abstract declaration of
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/taggedRelease/virtualvoid/(constvoid*)
 * taggedRelease(const void *)@/link</code>.
 *
 * @discussion
 * See
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/taggedRelease/virtualvoid/(constvoid*)
 * OSObject::taggedRelease(const void *)@/link</code>.
 */
// WAS:  virtual void _RESERVEDOSMetaClassBase1();
	virtual void taggedRelease(const void * tag = NULL) const = 0;

protected:
/*!
 * @function taggedRelease
 *
 * @abstract
 * Abstract declaration of
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/taggedRelease/virtualvoid/(constvoid*,constint)
 * taggedRelease(const void *, const int freeWhen)@/link</code>.
 *
 * @discussion
 * See
 * <code>@link
 * //apple_ref/cpp/instm/OSObject/taggedRelease/virtualvoid/(constvoid*,constint)
 * OSObject::taggedRelease(const void *, const int freeWhen)@/link</code>.
 */
// WAS:  virtual void _RESERVEDOSMetaClassBase2();
	virtual void taggedRelease(
		const void * tag,
		const int    freeWhen) const = 0;

public:
	virtual kern_return_t
	Dispatch(const IORPC rpc);

	kern_return_t
	Invoke(const IORPC rpc);

private:
#if APPLE_KEXT_VTABLE_PADDING
// Virtual Padding
#if defined(__arm64__) || defined(__arm__)
	virtual void _RESERVEDOSMetaClassBase0();
	virtual void _RESERVEDOSMetaClassBase1();
	virtual void _RESERVEDOSMetaClassBase2();
	virtual void _RESERVEDOSMetaClassBase3();
#endif /* defined(__arm64__) || defined(__arm__) */
	virtual void _RESERVEDOSMetaClassBase4();
	virtual void _RESERVEDOSMetaClassBase5();
	virtual void _RESERVEDOSMetaClassBase6();
	virtual void _RESERVEDOSMetaClassBase7();
#endif /* APPLE_KEXT_VTABLE_PADDING */
} APPLE_KEXT_COMPATIBILITY;


#ifdef XNU_KERNEL_PRIVATE
typedef bool (*OSMetaClassInstanceApplierFunction)(const OSObject * instance,
    void * context);
#endif /* XNU_KERNEL_PRIVATE */

/*!
 * @class OSMetaClass
 *
 * @abstract
 * OSMetaClass manages run-time type information
 * for Libkern and I/O Kit C++ classes.
 *
 * @discussion OSMetaClass manages run-time type information
 * for Libkern and I/O Kit C++ classes.
 * An instance of OSMetaClass exists for (nearly) every such C++ class,
 * keeping track of inheritance relationships, class lookup by name,
 * instance counts, and more.
 * OSMetaClass operates almost entirely behind the scenes,
 * and kernel extensions should rarely, if ever,
 * have to interact directly with OSMetaClass.
 *
 * <b>Use by Kernel Extensions</b>
 *
 * While kernel extensions rarey interact directly with OSMetaClass at run time,
 * they must register their classes with the metaclass system
 * using the macros declared here.
 * The class declaration should use one of these two macros
 * before its first member function declaration:
 * <ul>
 * <li><code>@link OSDeclareDefaultStructors OSDeclareDefaultStructors@/link</code> -
 *     for classes with no abstract member function declarations</li>
 * <li><code>@link OSDeclareAbstractStructors OSDeclareAbstractStructors@/link</code> -
 *     for classes with at least one abstract member function declaration</li>
 * <li><code>@link OSDeclareFinalStructors OSDeclareFinalStructors@/link</code> -
 *     for classes that should not be subclassable by another kext</li>
 * </ul>
 *
 * The class implementation should then use one of these macros:
 * <ul>
 * <li><code>@link OSDefineMetaClassAndStructors
 *           OSDefineMetaClassAndStructors@/link</code> -
 *     for classes with no abstract member function declarations</li>
 * <li><code>@link OSDefineMetaClassAndAbstractStructors
 *           OSDefineMetaClassAndAbstractStructors@/link</code> -
 *     for classes with at least one abstract member function declaration</li>
 * <li><code>@link OSDefineMetaClassAndFinalStructors
 *           OSDefineMetaClassAndFinalStructors@/link</code> -
 *     for classes that should not be subclassable by another kext</li>
 * </ul>
 *
 * Classes in kernel extensions that are intended for use as libraries
 * may need to reserve vtable slots to preserve binary compatibility
 * as new functions are added. They may do so with these macros:
 * <ul>
 * <li><code>@link OSMetaClassDeclareReservedUnused
 *           OSMetaClassDeclareReservedUnused@/link</code> -
 *     reserves a vtable slot</li>
 * <li><code>@link OSMetaClassDefineReservedUnused
 *           OSMetaClassDefineReservedUnused@/link</code> -
 *     defines the reserved vtable slot as an unimplemented function</li>
 * <li><code>@link OSMetaClassDeclareReservedUsed
 *           OSMetaClassDeclareReservedUsed@/link</code> -
 *     documents that a formerly reserved slot is now used</li>
 * <li><code>@link OSMetaClassDefineReservedUsed
 *           OSMetaClassDefineReservedUsed@/link</code> -
 *    documents that a formerly reserved slot is now used</li>
 * </ul>
 *
 * <b>Use Restrictions</b>
 *
 * OSMetaClass should not be explicitly subclassed by kernel extensions
 * (the declare/define macros do that),
 * nor should kernel extensions call its run-time type functions directly.
 *
 * OSMetaClass functions should be considered
 * <b>unsafe</b> to call in a primary interrupt context.
 *
 * <b>Concurrency Protection</b>
 *
 * Kernel extensions should in general not interact
 * with OSMetaClass objects directly,
 * instead using the run-time type macros.
 * Much of OSMetaClass's interface is intended for use
 * by the run-time type information system,
 * which handles concurrency and locking internally.
 */
