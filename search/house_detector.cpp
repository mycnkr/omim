#include "house_detector.hpp"
#include "search_common.hpp"

#include "../indexer/feature_impl.hpp"
#include "../indexer/classificator.hpp"

#include "../geometry/distance.hpp"
#include "../geometry/distance_on_sphere.hpp"
#include "../geometry/angles.hpp"

#include "../base/logging.hpp"
#include "../base/stl_iterator.hpp"
#include "../base/limited_priority_queue.hpp"

#include "../std/set.hpp"
#include "../std/bind.hpp"
#include "../std/numeric.hpp"

#ifdef DEBUG
#include "../platform/platform.hpp"

#include "../std/iostream.hpp"
#include "../std/fstream.hpp"
#endif


namespace search
{

namespace
{

#ifdef DEBUG
void Houses2KML(ostream & s, map<search::House, double> const & m)
{
  for (map<search::House, double>::const_iterator it = m.begin(); it != m.end(); ++it)
  {
    m2::PointD const & pt = it->first.GetPosition();

    s << "<Placemark>"
      << "<name>" << it->first.GetNumber() <<  "</name>"

      << "<Point><coordinates>"
            << MercatorBounds::XToLon(pt.x)
            << ","
            << MercatorBounds::YToLat(pt.y)

      << "</coordinates></Point>"
      << "</Placemark>" << endl;
  }
}

void Street2KML(ostream & s, vector<m2::PointD> const & pts, char const * color)
{
  s << "<Placemark>" << endl;
  s << "<Style><LineStyle><color>" << color << "</color></LineStyle></Style>" << endl;

  s << "<LineString><coordinates>" << endl;
  for (size_t i = 0; i < pts.size(); ++i)
  {
    s << MercatorBounds::XToLon(pts[i].x) << "," << MercatorBounds::YToLat(pts[i].y) << "," << "0.0" << endl;
  }
  s << "</coordinates></LineString>" << endl;

  s << "</Placemark>" << endl;
}

void Streets2KML(ostream & s, MergedStreet const & st, char const * color)
{
  for (size_t i = 0; i < st.m_cont.size(); ++i)
    Street2KML(s, st.m_cont[i]->m_points, color);
}

class KMLFileGuard
{
  ofstream m_file;
public:
  KMLFileGuard(string const & name)
  {
    m_file.open(GetPlatform().WritablePathForFile(name).c_str());

    m_file << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" << endl;
    m_file << "<kml xmlns=\"http://earth.google.com/kml/2.2\">" << endl;
    m_file << "<Document>" << endl;
  }

  ostream & GetStream() { return m_file; }

  ~KMLFileGuard()
  {
    m_file << "</Document></kml>" << endl;
  }
};
#endif

/// @todo Move prefixes, suffixes into separate file (autogenerated).
/// "Набережная" улица встречается в городах

string affics1[] =
{
  "аллея", "бульвар", "набережная", "переулок",
  "площадь", "проезд", "проспект", "шоссе",
  "тупик", "улица", "тракт"
};

string affics2[] =
{
  "ал", "бул", "наб", "пер",
  "пл", "пр", "просп", "ш",
  "туп", "ул", "тр"
};

void GetStreetName(strings::SimpleTokenizer iter, string & streetName)
{
  while (iter)
  {
    string const s = strings::MakeLowerCase(*iter);
    ++iter;

    bool flag = true;
    for (size_t i = 0; i < ARRAY_SIZE(affics2); ++i)
    {
      if (s == affics2[i] || s == affics1[i])
      {
        flag = false;
        break;
      }
    }

    if (flag)
      streetName += s;
  }
}

int GetIntHouse(string const & s)
{
  char const * start = s.c_str();
  char * stop;
  long const x = strtol(start, &stop, 10);
  return (stop == start ? -1 : x);
}

double const STREET_CONNECTION_LENGTH_M = 100.0;

class StreetCreator
{
  Street * m_street;
public:
  StreetCreator(Street * st) : m_street(st) {}
  void operator () (CoordPointT const & point) const
  {
    m_street->m_points.push_back(m2::PointD(point.first, point.second));
  }
};

}

void GetStreetNameAsKey(string const & name, string & res)
{
  strings::SimpleTokenizer iter(name, "\t -,.");
  GetStreetName(iter, res);
}

void House::InitHouseNumber()
{
  strings::SimpleTokenizer it(m_number, ",-; ");
  while (it)
  {
    int const number = GetIntHouse(*it);
    if (number != -1)
    {
      if (m_startN == -1)
        m_startN = number;
      else
      {
        // always assign to get house number boundaries [176, 182]
        m_endN = number;
      }
    }

    ++it;
  }

  ASSERT_GREATER_OR_EQUAL(m_startN, 0, (m_number));

  if (m_endN != -1 && m_startN > m_endN)
    swap(m_startN, m_endN);
}

House::ParsedNumber::ParsedNumber(string const & number)
  : m_fullN(&number), m_intN(GetIntHouse(number))
{
}

int House::GetMatch(ParsedNumber const & number) const
{
  if (((m_endN == -1) && m_startN != number.m_intN) ||
      ((m_endN != -1) && (m_startN > number.m_intN || m_endN < number.m_intN)))
  {
    return -1;
  }

  if (*number.m_fullN == m_number)
    return 0;

  if ((number.m_intN % 2 == 0) == (m_startN % 2 == 0))
    return 1;

  return 2;
}

FeatureLoader::FeatureLoader(Index const * pIndex)
  : m_pIndex(pIndex), m_pGuard(0)
{
}

FeatureLoader::~FeatureLoader()
{
  Free();
}

void FeatureLoader::CreateLoader(size_t mwmID)
{
  if (m_pGuard == 0 || mwmID != m_pGuard->GetID())
  {
    delete m_pGuard;
    m_pGuard = new Index::FeaturesLoaderGuard(*m_pIndex, mwmID);
  }
}

void FeatureLoader::Load(FeatureID const & id, FeatureType & f)
{
  CreateLoader(id.m_mwm);
  m_pGuard->GetFeature(id.m_offset, f);
}

void FeatureLoader::Free()
{
  delete m_pGuard;
  m_pGuard = 0;
}

template <class ToDo>
void FeatureLoader::ForEachInRect(m2::RectD const & rect, ToDo toDo)
{
  m_pIndex->ForEachInRect(toDo, rect, scales::GetUpperScale());
}

m2::RectD Street::GetLimitRect(double offsetMeters) const
{
  m2::RectD rect;
  for (size_t i = 0; i < m_points.size(); ++i)
    rect.Add(MercatorBounds::RectByCenterXYAndSizeInMeters(m_points[i], offsetMeters));
  return rect;
}

void Street::SetName(string const & name)
{
  m_name = name;
  GetStreetNameAsKey(name, m_processedName);
}

namespace
{

bool LessStreetDistance(HouseProjection const & p1, HouseProjection const & p2)
{
  return p1.m_streetDistance < p2.m_streetDistance;
}

double GetDistanceMeters(m2::PointD const & p1, m2::PointD const & p2)
{
  return ms::DistanceOnEarth(MercatorBounds::YToLat(p1.y), MercatorBounds::XToLon(p1.x),
                             MercatorBounds::YToLat(p2.y), MercatorBounds::XToLon(p2.x));
}

pair<double, double> GetConnectionAngleAndDistance(bool & isBeg, Street const * s1, Street const * s2)
{
  m2::PointD const & p1 = isBeg ? s1->m_points.front() : s1->m_points.back();
  m2::PointD const & p0 = isBeg ? s1->m_points[1] : s1->m_points[s1->m_points.size()-2];

  double const d0 = p1.SquareLength(s2->m_points.front());
  double const d2 = p1.SquareLength(s2->m_points.back());
  isBeg = (d0 < d2);
  m2::PointD const & p2 = isBeg ? s2->m_points[1] : s2->m_points[s2->m_points.size()-2];

  return make_pair(ang::GetShortestDistance(ang::AngleTo(p0, p1), ang::AngleTo(p1, p2)), min(d0, d2));
}

}

void Street::Reverse()
{
  ASSERT(m_houses.empty(), ());
  reverse(m_points.begin(), m_points.end());
}

void Street::SortHousesProjection()
{
  sort(m_houses.begin(), m_houses.end(), &LessStreetDistance);
}

HouseDetector::HouseDetector(Index const * pIndex)
  : m_loader(pIndex), m_streetNum(0)
{
  // default value for conversions
  SetMetres2Mercator(360.0 / 40.0E06);
}

void HouseDetector::SetMetres2Mercator(double factor)
{
  m_metres2Mercator = factor;

  LOG(LDEBUG, ("Street join epsilon = ", m_metres2Mercator * STREET_CONNECTION_LENGTH_M));
}

double HouseDetector::GetApprLengthMeters(int index) const
{
  m2::PointD const & p1 = m_streets[index].m_cont.front()->m_points.front();
  m2::PointD const & p2 = m_streets[index].m_cont.back()->m_points.back();
  return p1.Length(p2) / m_metres2Mercator;
}

HouseDetector::StreetPtr HouseDetector::FindConnection(Street const * st, bool beg) const
{
  m2::PointD const & pt = beg ? st->m_points.front() : st->m_points.back();
  double const maxAngle = math::pi/2.0;

  StreetPtr resStreet(0, false);
  double resDistance = numeric_limits<double>::max();
  double const minSqDistance = math::sqr(m_metres2Mercator * STREET_CONNECTION_LENGTH_M);

  for (size_t i = 0; i < m_end2st.size(); ++i)
  {
    if (pt.SquareLength(m_end2st[i].first) > minSqDistance)
      continue;

    Street * current = m_end2st[i].second;

    // Choose the possible connection from non-processed and from the same street parts.
    if (current != st && (current->m_number == -1 || current->m_number == m_streetNum) &&
        Street::IsSameStreets(st, current))
    {
      // Choose the closest connection with suitable angle.
      bool isBeg = beg;
      pair<double, double> const res = GetConnectionAngleAndDistance(isBeg, st, current);
      if (fabs(res.first) < maxAngle && res.second < resDistance)
      {
        resStreet = StreetPtr(current, isBeg);
        resDistance = res.second;
      }
    }
  }

  if (resStreet.first && resStreet.first->m_number == -1)
    return resStreet;
  else
    return StreetPtr(0, false);
}

void HouseDetector::MergeStreets(Street * st)
{
  st->m_number = m_streetNum;

  m_streets.push_back(MergedStreet());
  MergedStreet & ms = m_streets.back();
  ms.m_cont.push_back(st);

  bool isBeg = true;
  while (true)
  {
    // find connection from begin or end
    StreetPtr st(0, false);
    if (isBeg)
      st = FindConnection(ms.m_cont.front(), true);
    if (st.first == 0)
    {
      isBeg = false;
      st = FindConnection(ms.m_cont.back(), false);
      if (st.first == 0)
        return;
    }

    if (isBeg == st.second)
      st.first->Reverse();

    st.first->m_number = m_streetNum;

    if (isBeg)
      ms.m_cont.push_front(st.first);
    else
      ms.m_cont.push_back(st.first);
  }
}

int HouseDetector::LoadStreets(vector<FeatureID> const & ids)
{
  //LOG(LDEBUG, ("IDs = ", ids));

  ASSERT(IsSortedAndUnique(ids.begin(), ids.end()), ());

  // Check if the cache is obsolete and need to be cleared.
  if (!m_id2st.empty())
  {
    typedef pair<FeatureID, Street *> ValueT;
    function<ValueT::first_type const & (ValueT const &)> f = bind(&ValueT::first, _1);

    CounterIterator it = set_difference(ids.begin(), ids.end(),
                                        make_transform_iterator(m_id2st.begin(), f),
                                        make_transform_iterator(m_id2st.end(), f),
                                        CounterIterator());
    if (it.GetCount() > ids.size() / 2)
    {
      LOG(LDEBUG, ("Clear HouseDetector cache: missed", it.GetCount(), "of", ids.size(), "elements."));
      ClearCaches();
    }
  }

  // Load streets.
  int count = 0;
  for (size_t i = 0; i < ids.size(); ++i)
  {
    if (m_id2st.find(ids[i]) != m_id2st.end())
      continue;

    FeatureType f;
    m_loader.Load(ids[i], f);
    if (f.GetFeatureType() == feature::GEOM_LINE)
    {
      /// @todo Assume that default name always exist as primary compare key.
      string name;
      if (!f.GetName(0, name) || name.empty())
        continue;

      ++count;

      Street * st = new Street();
      st->SetName(name);
      f.ForEachPoint(StreetCreator(st), FeatureType::BEST_GEOMETRY);

      if (m_end2st.empty())
      {
        m2::PointD const p1 = st->m_points.front();
        m2::PointD const p2 = st->m_points.back();

        SetMetres2Mercator(p1.Length(p2) / GetDistanceMeters(p1, p2));
      }

      m_id2st[ids[i]] = st;
      m_end2st.push_back(make_pair(st->m_points.front(), st));
      m_end2st.push_back(make_pair(st->m_points.back(), st));
    }
  }

  m_loader.Free();
  return count;
}

int HouseDetector::MergeStreets()
{
  LOG(LDEBUG, ("MergeStreets() called", m_id2st.size()));

//#ifdef DEBUG
//  KMLFileGuard file("dbg_merged_streets.kml");
//#endif

  for (IterM it = m_id2st.begin(); it != m_id2st.end(); ++it)
  {
    Street * st = it->second;

    if (st->m_number == -1)
    {
      MergeStreets(st);
      m_streetNum++;
    }
  }

//#ifdef DEBUG
//  char const * arrColor[] = { "FFFF0000", "FF00FFFF", "FFFFFF00", "FF0000FF", "FF00FF00", "FFFF00FF" };
//  for (size_t i = 0; i < m_streets.size(); ++i)
//  {
//    Streets2KML(file.GetStream(), m_streets[i], arrColor[i % ARRAY_SIZE(arrColor)]);
//  }
//#endif

  LOG(LDEBUG, ("MergeStreets() result", m_streetNum));
  return m_streetNum;
}

namespace
{

class ProjectionCalcToStreet
{
  vector<m2::PointD> const & m_points;
  double m_distanceMeters;

  typedef m2::ProjectionToSection<m2::PointD> ProjectionT;
  vector<ProjectionT> m_calcs;

public:
  ProjectionCalcToStreet(Street const * st, double distanceMeters)
    : m_points(st->m_points), m_distanceMeters(distanceMeters)
  {
    ASSERT_GREATER(m_points.size(), 1, ());
  }

  void Initialize()
  {
    if (m_calcs.empty())
    {
      size_t const count = m_points.size() - 1;
      m_calcs.resize(count);
      for (size_t i = 0; i < count; ++i)
        m_calcs[i].SetBounds(m_points[i], m_points[i+1]);
    }
  }

  double GetLength(size_t ind) const
  {
    double length = 0.0;
    for (size_t i = 0; i < ind; ++i)
      length += m_calcs[i].GetLength();
    return length;
  }

  double GetLength()
  {
    Initialize();
    return GetLength(m_calcs.size());
  }

  void CalculateProjectionParameters(m2::PointD const & pt,
                                     m2::PointD & resPt, double & dist, double & resDist, size_t & ind)
  {
    for (size_t i = 0; i < m_calcs.size(); ++i)
    {
      m2::PointD const proj = m_calcs[i](pt);
      dist = GetDistanceMeters(pt, proj);
      if (dist < resDist)
      {
        resPt = proj;
        resDist = dist;
        ind = i;
      }
    }
  }

  bool GetProjection(m2::PointD const & pt, HouseProjection & proj)
  {
    Initialize();

    m2::PointD resPt;
    double dist = numeric_limits<double>::max();
    double resDist = numeric_limits<double>::max();
    size_t ind;

    CalculateProjectionParameters(pt, resPt, dist, resDist, ind);

    if (resDist <= m_distanceMeters)
    {
      proj.m_proj = resPt;
      proj.m_distance = resDist;
      proj.m_streetDistance = GetLength(ind) + m_points[ind].Length(proj.m_proj);
      proj.m_projectionSign = m2::GetOrientation(m_points[ind], m_points[ind+1], pt) >= 0;
      return true;
    }
    else
      return false;
  }
};

}

string const & MergedStreet::GetDbgName() const
{
  ASSERT(!m_cont.empty(), ());
  return m_cont.front()->GetDbgName();
}

bool MergedStreet::IsHousesReaded() const
{
  ASSERT(!m_cont.empty(), ());
  return m_cont.front()->m_housesReaded;
}

void MergedStreet::Next(Index & i) const
{
  while (i.s < m_cont.size() && i.h == m_cont[i.s]->m_houses.size())
  {
    i.h = 0;
    ++i.s;
  }
}

void MergedStreet::Erase(Index & i)
{
  ASSERT(!IsEnd(i), ());
  m_cont[i.s]->m_houses.erase(m_cont[i.s]->m_houses.begin() + i.h);
  if (m_cont[i.s]->m_houses.empty())
    m_cont.erase(m_cont.begin() + i.s);
  Next(i);
}

void MergedStreet::FinishReadingHouses()
{
  // Correct m_streetDistance for each projection according to merged streets.
  double length = 0.0;
  for (size_t i = 0; i < m_cont.size(); ++i)
  {
    if (i != 0)
      for (size_t j = 0; j < m_cont[i]->m_houses.size(); ++j)
        m_cont[i]->m_houses[j].m_streetDistance += length;

    length += m_cont[i]->m_length;
    m_cont[i]->m_housesReaded = true;
  }

  // Unique projections for merged street.
  for (Index i = Begin(); !IsEnd(i);)
  {
    HouseProjection const & p1 = Get(i);
    bool incI = true;

    Index j = i; Inc(j);
    while (!IsEnd(j))
    {
      HouseProjection const & p2 = Get(j);
      if (p1.m_house == p2.m_house)
      {
        if (p1.m_distance < p2.m_distance)
        {
          Erase(j);
        }
        else
        {
          Erase(i);
          incI = false;
          break;
        }
      }
      else
        Inc(j);
    }

    if (incI)
      Inc(i);
  }
}

HouseProjection const * MergedStreet::GetHousePivot(bool & odd, bool & sign) const
{
  typedef my::limited_priority_queue<
      HouseProjection const *, HouseProjection::LessDistance> QueueT;
  QueueT q(16);

  // Get some most closest houses.
  for (MergedStreet::Index i = Begin(); !IsEnd(i); Inc(i))
    q.push(&Get(i));

  // Calculate all probabilities.
  // even-left, odd-left, even-right, odd-right
  double counter[4] = { 0, 0, 0, 0 };
  for (QueueT::const_iterator i = q.begin(); i != q.end(); ++i)
  {
    size_t ind = (*i)->m_house->GetIntNumber() % 2;
    if ((*i)->m_projectionSign)
      ind += 2;

    // Needed min summary distance, but process max function.
    counter[ind] += (1.0 / (*i)->m_distance);
  }

  // Get best odd-sign pair.
  if (counter[0] + counter[3] > counter[1] + counter[2])
  {
    odd = true;
    sign = true;
  }
  else
  {
    odd = false;
    sign = true;
  }

  // Get result pivot according to odd-sign pair.
  while (!q.empty())
  {
    HouseProjection const * p = q.top();
    if ((p->m_projectionSign == sign) == (p->IsOdd() == odd))
      return p;
    q.pop();
  }

  return 0;
}

template <class ProjectionCalcT>
void HouseDetector::ReadHouse(FeatureType const & f, Street * st, ProjectionCalcT & calc)
{
  static ftypes::IsBuildingChecker checker;

  string const houseNumber = f.GetHouseNumber();
  if (checker(f) && feature::IsHouseNumber(houseNumber))
  {
    map<FeatureID, House *>::iterator const it = m_id2house.find(f.GetID());

    // 15 - is a minimal building scale (enough for center point).
    m2::PointD const pt = (it == m_id2house.end()) ?
          f.GetLimitRect(15).Center() : it->second->GetPosition();

    HouseProjection pr;
    if (calc.GetProjection(pt, pr))
    {
      House * p;
      if (it == m_id2house.end())
      {
        p = new House(houseNumber, pt);
        m_id2house[f.GetID()] = p;
      }
      else
      {
        p = it->second;
        ASSERT(p != 0, ());
      }

      pr.m_house = p;
      st->m_houses.push_back(pr);
    }
  }
}

void HouseDetector::ReadHouses(Street * st, double offsetMeters)
{
  if (st->m_housesReaded)
    return;

  offsetMeters = max(50.0, min(GetApprLengthMeters(st->m_number) / 2, offsetMeters));

  ProjectionCalcToStreet calcker(st, offsetMeters);
  m_loader.ForEachInRect(st->GetLimitRect(offsetMeters),
                         bind(&HouseDetector::ReadHouse<ProjectionCalcToStreet>, this, _1, st, ref(calcker)));

  st->m_length = calcker.GetLength();
  st->SortHousesProjection();
}

void HouseDetector::ReadAllHouses(double offsetMeters)
{
  for (IterM it = m_id2st.begin(); it != m_id2st.end(); ++it)
    ReadHouses(it->second, offsetMeters);

  for (size_t i = 0; i < m_streets.size(); ++i)
  {
    if (!m_streets[i].IsHousesReaded())
      m_streets[i].FinishReadingHouses();
  }
}

void HouseDetector::ClearCaches()
{
  for (IterM it = m_id2st.begin(); it != m_id2st.end();++it)
    delete it->second;
  m_id2st.clear();

  for (map<FeatureID, House *>::iterator it = m_id2house.begin(); it != m_id2house.end(); ++it)
    delete it->second;

  m_streetNum = 0;

  m_id2house.clear();
  m_end2st.clear();
  m_streets.clear();
}

namespace
{

struct LS
{
  size_t prevDecreasePos, decreaseValue;
  size_t prevIncreasePos, increaseValue;

  LS() {}
  LS(size_t i)
  {
    prevDecreasePos = i;
    decreaseValue = 1;
    prevIncreasePos = i;
    increaseValue = 1;
  }
};

void LongestSubsequence(vector<HouseProjection const *> const & houses,
                        vector<HouseProjection const *> & result)
{
  result.clear();

  size_t const count = houses.size();
  if (count < 2)
  {
    result = houses;
    return;
  }

  vector<LS> v(count);
  for (size_t i = 0; i < count; ++i)
    v[i] = LS(i);

  House::LessHouseNumber cmp;
  size_t res = 0;
  size_t pos = 0;
  for (size_t i = 0; i < count - 1; ++i)
  {
    for (size_t j = i + 1; j < count; ++j)
    {
      bool const cmpIJ = cmp(houses[i]->m_house, houses[j]->m_house);

      // If equal houses
      if (cmpIJ == cmp(houses[j]->m_house, houses[i]->m_house))
      {
        ASSERT(!cmpIJ, ());
        continue;
      }

      if (cmpIJ && v[i].increaseValue + 1 >= v[j].increaseValue)
      {
        if (v[i].increaseValue + 1 == v[j].increaseValue && houses[v[j].prevIncreasePos]->m_distance < houses[i]->m_distance)
          continue;

        v[j].increaseValue = v[i].increaseValue + 1;
        v[j].prevIncreasePos = i;
      }

      if (!cmpIJ && v[i].decreaseValue + 1 >= v[j].decreaseValue)
      {
        if (v[i].decreaseValue + 1 == v[j].decreaseValue && houses[v[j].prevDecreasePos]->m_distance < houses[i]->m_distance)
          continue;

        v[j].decreaseValue = v[i].decreaseValue + 1;
        v[j].prevDecreasePos = i;
      }

      size_t const m = max(v[j].increaseValue, v[j].decreaseValue);
      if (m > res)
      {
        res = m;
        pos = j;
      }
    }
  }

  result.resize(res);
  bool increasing = true;
  if (v[pos].increaseValue < v[pos].decreaseValue)
    increasing = false;

  while (res > 0)
  {
    result[res - 1] = houses[pos];
    --res;
    if (increasing)
      pos = v[pos].prevIncreasePos;
    else
      pos = v[pos].prevDecreasePos;
  }
}

typedef map<search::House const *, double, House::LessHouseNumber> HouseMapT;

void AddHouseToMap(HouseProjection const * proj, HouseMapT & m)
{
  HouseMapT::iterator it = m.find(proj->m_house);
  if (it != m.end())
  {
    ASSERT_EQUAL(proj->m_house->GetIntNumber(), it->first->GetIntNumber(), ());

    if (it->second > proj->m_distance)
      m.erase(it);
    else
      return;
  }
  m.insert(make_pair(proj->m_house, proj->m_distance));
}

void ProccessHouses(vector<HouseProjection const *> & houses, HouseMapT & m)
{
  vector<HouseProjection const *> result;
  LongestSubsequence(houses, result);

  for_each(result.begin(), result.end(), bind(&AddHouseToMap, _1, ref(m)));
}

House const * GetClosestHouse(MergedStreet const & st, string const & houseNumber)
{
  double dist = numeric_limits<double>::max();
  int streetIndex = -1;
  int houseIndex = -1;

  bool isOdd, sign;
  if (st.GetHousePivot(isOdd, sign) == 0)
    return 0;

  House::ParsedNumber parsedNumber(houseNumber);
  for (size_t i = 0; i < st.size(); ++i)
  {
    for (size_t j = 0; j < st[i]->m_houses.size(); ++j)
    {
      bool s = st[i]->m_houses[j].m_projectionSign;
      bool odd = st[i]->m_houses[j].m_house->GetIntNumber() % 2 == 1;

      /// @todo Need to split distance and full house name match.
      if (((isOdd == odd) == (sign == s)) &&
          (st[i]->m_houses[j].m_distance < dist) &&
          (st[i]->m_houses[j].m_house->GetMatch(parsedNumber) != -1))
      {
        dist = st[i]->m_houses[j].m_distance;
        streetIndex = i;
        houseIndex = j;
      }
    }
  }

  if (streetIndex == -1)
    return 0;
  return st[streetIndex]->m_houses[houseIndex].m_house;
}

struct ScoredHouse
{
  House const * house;
  double score;
  ScoredHouse(House const * h, double s):house(h), score(s)
  {}
  ScoredHouse():house(0),score(numeric_limits<double>::max())
  {}
};

void addToQueue(int houseNumber, queue<int> & q)
{
  q.push(houseNumber + 2);
  if (houseNumber - 2 > 0)
    q.push(houseNumber - 2);
  if (houseNumber - 4 > 0)
    q.push(houseNumber - 4);
  q.push(houseNumber + 4);
}

struct HouseChain
{
  vector<HouseProjection const *> houses;
  set<string> s;
  double score;
  int minHouseNumber;
  int maxHouseNumber;

  HouseChain()
  {
    minHouseNumber = -1;
    maxHouseNumber = numeric_limits<int>::max();
  }

  HouseChain(HouseProjection const * h)
  {
    minHouseNumber = maxHouseNumber = h->m_house->GetIntNumber();
    Add(h);
  }

  void Add(HouseProjection const * h)
  {
    if (s.insert(h->m_house->GetNumber()).second)
    {
      int num = h->m_house->GetIntNumber();
      if (num < minHouseNumber)
        minHouseNumber = num;
      if (num > maxHouseNumber)
        maxHouseNumber = num;
      houses.push_back(h);
    }
  }

  bool Find(string const & str)
  {
    return (s.find(str) != s.end());
  }

  void CountScore()
  {
    sort(houses.begin(), houses.end(), HouseProjection::LessDistance());
    size_t const s = min(houses.size(), size_t(3));
    score = 0;
    for (size_t i = 0; i < s; ++i)
      score += houses[i]->m_distance;
    score /= s;
  }

  bool operator<(HouseChain const & p) const
  {
    return score < p.score;
  }
};

const int maxHouseNumberDistance = 4;
const double maxHouseConnectionDistance = 300.0;

ScoredHouse GetBestHouseFromChains(vector<HouseChain> & houseChains, string const & houseNumber)
{
  for (size_t i = 0; i < houseChains.size(); ++i)
    houseChains[i].CountScore();
  sort(houseChains.begin(), houseChains.end());

  for (size_t i = 1; i < houseChains.size(); ++i)
  {
    if (houseChains[i].score >= 300)
      break;
    for (size_t j = 0; j < houseChains[i].houses.size(); ++j)
      houseChains[0].Add(houseChains[i].houses[j]);
  }

  for (size_t j = 0; j < houseChains[0].houses.size(); ++j)
    if (houseNumber == houseChains[0].houses[j]->m_house->GetNumber())
      return ScoredHouse(houseChains[0].houses[j]->m_house, houseChains[0].score);
  return ScoredHouse(0, numeric_limits<double>::max());
}


ScoredHouse ProccessHouses(vector<HouseProjection const *> const & st, string const & houseNumber)
{
  vector<HouseChain> houseChains;
  size_t const count = st.size();
  size_t numberOfStreetHouses = count;
  vector<bool> used(count, false);

  for (size_t i = 0; i < count; ++i)
  {
    HouseProjection const * hp = st[i];
    if (st[i]->m_house->GetNumber() == houseNumber)
    {
      houseChains.push_back(HouseChain(hp));
      used[i] = true;
      --numberOfStreetHouses;
    }
  }
  if (houseChains.empty())
    return ScoredHouse();

  queue<int> houseNumbersToCheck;
  addToQueue(houseChains[0].houses[0]->m_house->GetIntNumber(), houseNumbersToCheck);
  while (numberOfStreetHouses > 0)
  {
    if (!houseNumbersToCheck.empty())
    {
      int candidateHouseNumber = houseNumbersToCheck.front();
      houseNumbersToCheck.pop();
      vector <int> candidates;
      for (size_t i = 0; i < used.size(); ++i)
        if (!used[i] && st[i]->m_house->GetIntNumber() == candidateHouseNumber)
          candidates.push_back(i);
      int numberOfCandidates = candidates.size();

      bool shouldAddHouseToQueue = false;

      while (numberOfCandidates > 0)
      {
        double dist = numeric_limits<double>::max();
        int candidateIndex = -1;
        int chainIndex = -1;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
          string num = st[candidates[i]]->m_house->GetNumber();
          if (!used[candidates[i]])
          {
            for (size_t j = 0; j < houseChains.size(); ++j)
              for (size_t k = 0; k < houseChains[j].houses.size(); ++k)
                if (!houseChains[j].Find(num) &&
                    abs(houseChains[j].houses[k]->m_house->GetIntNumber() - candidateHouseNumber) <= maxHouseNumberDistance)
                {
                  double tmp = GetDistanceMeters(houseChains[j].houses[k]->m_house->GetPosition(), st[candidates[i]]->m_house->GetPosition());
                  if (tmp < maxHouseConnectionDistance && tmp < dist)
                  {
                    dist = tmp;
                    chainIndex = j;
                    candidateIndex = i;
                  }
                }
          }
        }
        if (chainIndex != -1)
        {
          houseChains[chainIndex].Add(st[candidates[candidateIndex]]);
          shouldAddHouseToQueue = true;
        }
        else
        {
          for (size_t i = 0; i < candidates.size(); ++i)
            if (!used[candidates[i]])
            {
              houseChains.push_back(HouseChain(st[candidates[i]]));
              used[candidates[i]] = true;
              --numberOfStreetHouses;
              break;
            }
          numberOfCandidates = 0;
          break;
        }

        used[candidates[candidateIndex]] = true;
        --numberOfStreetHouses;
        --numberOfCandidates;
      }
      if (shouldAddHouseToQueue)
        addToQueue(candidateHouseNumber, houseNumbersToCheck);
    }
    else
    {
      for (size_t i = 0; i < used.size(); ++i)
      {
        if (!used[i])
        {
          houseChains.push_back(HouseChain(st[i]));
          --numberOfStreetHouses;
          used[i] = true;
          addToQueue(st[i]->m_house->GetIntNumber(), houseNumbersToCheck);
          break;
        }
      }
    }
  }

  return GetBestHouseFromChains(houseChains, houseNumber);
}

House const * GetBestHouseWithNumber(MergedStreet const & st, string const & houseNumber)
{
  bool isOdd, sign;
  if (st.GetHousePivot(isOdd, sign) == 0)
    return 0;

  vector<HouseProjection const *> v1, v2;
  for (MergedStreet::Index i = st.Begin(); !st.IsEnd(i); st.Inc(i))
  {
    HouseProjection const & proj = st.Get(i);
    if (proj.m_projectionSign == sign && proj.IsOdd() == isOdd)
      v2.push_back(&proj);
    else if (proj.m_projectionSign != sign && proj.IsOdd() != isOdd)
      v1.push_back(&proj);
  }

  ScoredHouse s1 = ProccessHouses(v1, houseNumber);
  ScoredHouse s2 = ProccessHouses(v2, houseNumber);
  return (s1.score < s2.score ? s1.house : s2.house);
}

House const * GetLSHouse(MergedStreet const & st, string const & houseNumber, HouseMapT & m)
{
  bool isOdd, sign;
  HouseProjection const * pivot = st.GetHousePivot(isOdd, sign);
  if (pivot == 0)
    return 0;

  m.insert(make_pair(pivot->m_house, 0));

  vector<HouseProjection const *> v1, v2;
  for (MergedStreet::Index i = st.Begin(); !st.IsEnd(i); st.Inc(i))
  {
    search::HouseProjection const & p = st.Get(i);
    if (p.m_projectionSign == sign && p.IsOdd() == isOdd)
      v1.push_back(&p);
    else if (p.m_projectionSign != sign && p.IsOdd() != isOdd)
      v2.push_back(&p);
  }

  ProccessHouses(v1, m);
  ProccessHouses(v2, m);

  House const * arrRes[] = { 0, 0, 0 };
  House::ParsedNumber parsedNumber(houseNumber);
  for (HouseMapT::iterator it = m.begin(); it != m.end(); ++it)
  {
    int const i = it->first->GetMatch(parsedNumber);
    if (i >= 0)
      arrRes[i] = it->first;
  }

  for (size_t i = 0; i < ARRAY_SIZE(arrRes); ++i)
    if (arrRes[i])
      return arrRes[i];
  return 0;
}

}

void HouseDetector::GetHouseForName(string const & houseNumber, vector<House const *> & res)
{
  size_t const count = m_streets.size();
  res.reserve(count);

  LOG(LDEBUG, ("Streets count", count));

  for (size_t i = 0; i < count; ++i)
  {
    LOG(LDEBUG, (m_streets[i].GetDbgName()));

    House const * house = 0;
    HouseMapT m;

    //house = GetLSHouse(m_streets[i], houseNumber, m);
    house = GetBestHouseWithNumber(m_streets[i], houseNumber);
    //house = GetClosestHouse(m_streets[i], houseNumber);

    if (house)
      res.push_back(house);
  }

  sort(res.begin(), res.end());
  res.erase(unique(res.begin(), res.end()), res.end());
}

}
