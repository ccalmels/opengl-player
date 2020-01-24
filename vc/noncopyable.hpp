class noncopyable {
private:
	noncopyable(const noncopyable &) = delete;
	noncopyable & operator = (const noncopyable &) = delete;
public:
	noncopyable() = default;
	~noncopyable() = default;
};
