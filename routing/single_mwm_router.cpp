#include "routing/single_mwm_router.hpp"

#include "routing/base/astar_algorithm.hpp"
#include "routing/base/astar_progress.hpp"
#include "routing/bicycle_directions.hpp"
#include "routing/bicycle_model.hpp"
#include "routing/car_model.hpp"
#include "routing/index_graph.hpp"
#include "routing/index_graph_serialization.hpp"
#include "routing/index_graph_starter.hpp"
#include "routing/pedestrian_model.hpp"
#include "routing/restriction_loader.hpp"
#include "routing/route.hpp"
#include "routing/routing_helpers.hpp"
#include "routing/turns_generator.hpp"
#include "routing/vehicle_mask.hpp"

#include "indexer/feature_altitude.hpp"

#include "geometry/distance.hpp"
#include "geometry/mercator.hpp"
#include "geometry/point2d.hpp"

#include "base/exception.hpp"

#include "std/algorithm.hpp"

using namespace routing;

namespace
{
size_t constexpr kMaxRoadCandidates = 6;
float constexpr kProgressInterval = 2;
uint32_t constexpr kDrawPointsPeriod = 10;
}  // namespace

namespace routing
{
SingleMwmRouter::SingleMwmRouter(string const & name, Index const & index,
                                 traffic::TrafficCache const & trafficCache,
                                 shared_ptr<VehicleModelFactory> vehicleModelFactory,
                                 shared_ptr<EdgeEstimator> estimator,
                                 unique_ptr<IDirectionsEngine> directionsEngine)
  : m_name(name)
  , m_index(index)
  , m_trafficCache(trafficCache)
  , m_roadGraph(index, IRoadGraph::Mode::ObeyOnewayTag, vehicleModelFactory)
  , m_vehicleModelFactory(vehicleModelFactory)
  , m_estimator(estimator)
  , m_directionsEngine(move(directionsEngine))
{
  ASSERT(!m_name.empty(), ());
  ASSERT(m_vehicleModelFactory, ());
  ASSERT(m_estimator, ());
  ASSERT(m_directionsEngine, ());
}

IRouter::ResultCode SingleMwmRouter::CalculateRoute(MwmSet::MwmId const & mwmId,
                                                    m2::PointD const & startPoint,
                                                    m2::PointD const & startDirection,
                                                    m2::PointD const & finalPoint,
                                                    RouterDelegate const & delegate, Route & route)
{
  try
  {
    return DoCalculateRoute(mwmId, startPoint, startDirection, finalPoint, delegate, route);
  }
  catch (RootException const & e)
  {
    LOG(LERROR, ("Can't find path from", MercatorBounds::ToLatLon(startPoint), "to",
                 MercatorBounds::ToLatLon(finalPoint), ":\n ", e.what()));
    return IRouter::InternalError;
  }
}

IRouter::ResultCode SingleMwmRouter::DoCalculateRoute(MwmSet::MwmId const & mwmId,
                                                      m2::PointD const & startPoint,
                                                      m2::PointD const & /* startDirection */,
                                                      m2::PointD const & finalPoint,
                                                      RouterDelegate const & delegate,
                                                      Route & route)
{
  if (!mwmId.IsAlive())
    return IRouter::RouteFileNotExist;

  string const & country = mwmId.GetInfo()->GetCountryName();

  Edge startEdge;
  if (!FindClosestEdge(mwmId, startPoint, startEdge))
    return IRouter::StartPointNotFound;

  Edge finishEdge;
  if (!FindClosestEdge(mwmId, finalPoint, finishEdge))
    return IRouter::EndPointNotFound;

  RoadPoint const start(startEdge.GetFeatureId().m_index, startEdge.GetSegId());
  RoadPoint const finish(finishEdge.GetFeatureId().m_index, finishEdge.GetSegId());

  EstimatorGuard guard(mwmId, *m_estimator);

  IndexGraph graph(GeometryLoader::Create(
                       m_index, mwmId, m_vehicleModelFactory->GetVehicleModelForCountry(country)),
                   m_estimator);

  if (!LoadIndex(mwmId, country, graph))
    return IRouter::RouteFileNotExist;

  IndexGraphStarter starter(graph, start, finish);

  AStarProgress progress(0, 100);
  progress.Initialize(starter.GetPoint(start), starter.GetPoint(finish));

  uint32_t drawPointsStep = 0;
  auto onVisitVertex = [&](IndexGraphStarter::TVertexType const & from,
                           IndexGraphStarter::TVertexType const & to) {
    m2::PointD const & pointFrom = starter.GetPoint(from.GetCurr());
    m2::PointD const & pointTo = starter.GetPoint(to.GetCurr());

    auto const lastValue = progress.GetLastValue();
    auto const newValue = progress.GetProgressForBidirectedAlgo(pointFrom, pointTo);
    if (newValue - lastValue > kProgressInterval)
      delegate.OnProgress(newValue);
    if (drawPointsStep % kDrawPointsPeriod == 0)
      delegate.OnPointCheck(pointFrom);
    ++drawPointsStep;
  };

  AStarAlgorithm<IndexGraphStarter> algorithm;

  RoutingResult<IndexGraphStarter::TVertexType> routingResult;
  auto const resultCode =
      algorithm.FindPathBidirectional(starter, starter.GetStartVertex(), starter.GetFinishVertex(),
                                      routingResult, delegate, onVisitVertex);

  switch (resultCode)
  {
  case AStarAlgorithm<IndexGraphStarter>::Result::NoPath: return IRouter::RouteNotFound;
  case AStarAlgorithm<IndexGraphStarter>::Result::Cancelled: return IRouter::Cancelled;
  case AStarAlgorithm<IndexGraphStarter>::Result::OK:
    // Because A* works in another space, where each vertex is
    // actually a pair (previous vertex, current vertex), and start
    // and finish vertices are (start joint, start joint), (finish
    // joint, finish joint) correspondingly, we need to get back to
    // the original space.

    vector<Joint::Id> joints;
    for (auto const & u : routingResult.path)
      joints.emplace_back(u.GetCurr());

    // If there are at least two points on the shortest path, then the
    // last point is duplicated.  Imagine that the shortest path in
    // the original space is: [s, u, v, t].  Then, in the another
    // space, the shortest path will be: [(s, s), (s, u), (u, v), (v,
    // t), (t, t)]. After taking second part of the vertices, the
    // sequence is: [s, u, v, t, t], therefore, the last vertex is
    // duplicated. On the other hand, if the shortest path in the
    // original space is a single vertex [s] - a case when start and
    // finish vertices are the same, in the extended space the
    // shortest path is [(s, s)], and after taking the second part the
    // sequence is [s] - exactly what do we need.
    if (joints.size() >= 2)
    {
      CHECK_EQUAL(joints[joints.size() - 1], joints[joints.size() - 2], ());
      joints.pop_back();
    }

    if (!BuildRoute(mwmId, joints, delegate, startPoint, finalPoint, starter, route))
      return IRouter::InternalError;
    if (delegate.IsCancelled())
      return IRouter::Cancelled;
    return IRouter::NoError;
  }
}

bool SingleMwmRouter::FindClosestEdge(MwmSet::MwmId const & mwmId, m2::PointD const & point,
                                      Edge & closestEdge) const
{
  vector<pair<Edge, Junction>> candidates;
  m_roadGraph.FindClosestEdges(point, kMaxRoadCandidates, candidates);

  double minDistance = numeric_limits<double>::max();
  size_t minIndex = candidates.size();

  for (size_t i = 0; i < candidates.size(); ++i)
  {
    Edge const & edge = candidates[i].first;
    if (edge.GetFeatureId().m_mwmId != mwmId)
      continue;

    m2::DistanceToLineSquare<m2::PointD> squaredDistance;
    squaredDistance.SetBounds(edge.GetStartJunction().GetPoint(), edge.GetEndJunction().GetPoint());
    double const distance = squaredDistance(point);
    if (distance < minDistance)
    {
      minDistance = distance;
      minIndex = i;
    }
  }

  if (minIndex == candidates.size())
    return false;

  closestEdge = candidates[minIndex].first;
  return true;
}

bool SingleMwmRouter::LoadIndex(MwmSet::MwmId const & mwmId, string const & country,
                                IndexGraph & graph)
{
  MwmSet::MwmHandle mwmHandle = m_index.GetMwmHandleById(mwmId);
  if (!mwmHandle.IsAlive())
    return false;

  MwmValue const * mwmValue = mwmHandle.GetValue<MwmValue>();
  try
  {
    my::Timer timer;
    FilesContainerR::TReader reader(mwmValue->m_cont.GetReader(ROUTING_FILE_TAG));
    ReaderSource<FilesContainerR::TReader> src(reader);
    IndexGraphSerializer::Deserialize(graph, src, kCarMask);
    RestrictionLoader restrictionLoader(*mwmValue);
    if (restrictionLoader.HasRestrictions())
      graph.ApplyRestrictions(restrictionLoader.GetRestrictions());

    LOG(LINFO,
        (ROUTING_FILE_TAG, "section for", country, "loaded in", timer.ElapsedSeconds(), "seconds"));
    return true;
  }
  catch (Reader::OpenException const & e)
  {
    LOG(LERROR, ("File", mwmValue->GetCountryFileName(), "Error while reading", ROUTING_FILE_TAG,
                 "section:", e.Msg()));
    return false;
  }
}

bool SingleMwmRouter::BuildRoute(MwmSet::MwmId const & mwmId, vector<Joint::Id> const & joints,
                                 RouterDelegate const & delegate, m2::PointD const & start,
                                 m2::PointD const & finish, IndexGraphStarter & starter,
                                 Route & route) const
{
  vector<RoutePoint> routePoints;
  starter.RedressRoute(joints, routePoints);

  // ReconstructRoute removes equal points: do it self to match time indexes.
  // TODO: rework ReconstructRoute and remove all time indexes stuff.
  routePoints.erase(unique(routePoints.begin(), routePoints.end(),
                           [&](RoutePoint const & rp0, RoutePoint const & rp1) {
                             return starter.GetPoint(rp0.GetRoadPoint()) ==
                                    starter.GetPoint(rp1.GetRoadPoint());
                           }),
                    routePoints.end());

  vector<Junction> junctions;
  junctions.reserve(routePoints.size());

  // TODO: Use real altitudes for pedestrian and bicycle routing.
  for (RoutePoint const & routePoint : routePoints)
  {
    junctions.emplace_back(starter.GetPoint(routePoint.GetRoadPoint()),
                           feature::kDefaultAltitudeMeters);
  }

  shared_ptr<traffic::TrafficInfo::Coloring> trafficColoring = m_trafficCache.GetTrafficInfo(mwmId);

  auto const numJunctions = junctions.size();
  ReconstructRoute(m_directionsEngine.get(), m_roadGraph, trafficColoring, delegate, start, finish,
                   junctions, route);

  if (junctions.size() != numJunctions)
  {
    LOG(LERROR, ("ReconstructRoute changed junctions: size before", numJunctions, ", size after",
                 junctions.size()));
    return false;
  }

  // ReconstructRoute duplicates all points except start and finish.
  // Therefore one needs fix time indexes to fit reconstructed polyline.
  if (routePoints.size() < 2 || route.GetPoly().GetSize() + 2 != routePoints.size() * 2)
  {
    LOG(LERROR, ("Can't fix route times: polyline size =", route.GetPoly().GetSize(),
                 "route points size =", routePoints.size()));
    return false;
  }

  Route::TTimes times;
  times.reserve(route.GetPoly().GetSize());
  times.emplace_back(0, routePoints.front().GetTime());

  for (size_t i = 1; i < routePoints.size() - 1; ++i)
  {
    times.emplace_back(i * 2 - 1, routePoints[i].GetTime());
    times.emplace_back(i * 2, routePoints[i].GetTime());
  }

  times.emplace_back(route.GetPoly().GetSize() - 1, routePoints.back().GetTime());
  route.SetSectionTimes(move(times));
  return true;
}

// static
unique_ptr<SingleMwmRouter> SingleMwmRouter::CreateCarRouter(
    Index const & index, traffic::TrafficCache const & trafficCache)
{
  auto vehicleModelFactory = make_shared<CarModelFactory>();
  // @TODO Bicycle turn generation engine is used now. It's ok for the time being.
  // But later a special car turn generation engine should be implemented.
  auto directionsEngine = make_unique<BicycleDirectionsEngine>(index);
  auto estimator =
      EdgeEstimator::CreateForCar(*vehicleModelFactory->GetVehicleModel(), trafficCache);
  auto router =
      make_unique<SingleMwmRouter>("astar-bidirectional-car", index, trafficCache,
                                   move(vehicleModelFactory), estimator, move(directionsEngine));
  return router;
}
}  // namespace routing
