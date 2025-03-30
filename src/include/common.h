#pragma once


namespace Quanta {

	class UID
	{
	public:
		unsigned long long h;
		unsigned long long l;

		UID()
		{
			h = l = 0;
		}
		UID(unsigned long long h0, unsigned long long l0)
		{
			h = h0;
			l = l0;
		}
		bool IsNull()
		{
			return h == 0 && l == 0;
		}
		UID(const UID& f)
		{
			h = f.h;
			l = f.l;
		}

		UID& operator=(const UID& f)
		{
			h = f.h;
			l = f.l;
			return *this;
		}

		bool operator<(const UID& r) const
		{
			return (h < r.h || (h == r.h && l < r.l));
		}
		bool operator==(const UID& r) const
		{
			return (h == r.h && l == r.l);
		}
		bool operator!=(const UID& r) const
		{
			return (h != r.h || l != r.l);
		}
	};

}